#include <iostream>
#include <vector>
#include <memory>
#include <string>
#include <cstring>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <csignal>
#include <sstream>
#include <fstream>
#include <deque>
#include <map>
#include <algorithm>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <cstdio>
#include <ctime>
#include <cstdarg>
#include <chrono>
#include <hiredis/hiredis.h>
#include <sqlite3.h>
#include <map>
#include "chess_engine.h"
#include "tile_magic_engine.h"
#include "snake_engine.h"
#include "tank_engine.h"
#include "gun_engine.h"
// 需要 C++17 的 to_string（已默认）

#define MAX_EVENTS 1024
#define REQ_BUF 8192
#define MAX_HEADER 8192
#define MAX_FRAME (1 << 20)
#define MAX_UPLOAD (50 * 1024 * 1024)
#define MAX_CONN 500
#define WS_BUF (64 * 1024)

static std::atomic<bool> g_running{true};
static void sig_handler(int) { g_running = false; }

static bool set_nonblocking(int fd) {
    int f = fcntl(fd, F_GETFL);
    if (f < 0) return false;
    return fcntl(fd, F_SETFL, f | O_NONBLOCK) == 0;
}

// ==================== SHA1 ====================
static uint32_t sha1_rotl(uint32_t v, int b) { return (v << b) | (v >> (32 - b)); }

static void sha1_hash(const unsigned char* data, size_t len, unsigned char out[20]) {
    uint32_t h0=0x67452301,h1=0xEFCDAB89,h2=0x98BADCFE,h3=0x10325476,h4=0xC3D2E1F0;
    std::vector<unsigned char> msg(data,data+len);
    uint64_t bits=(uint64_t)len*8;
    msg.push_back(0x80);
    while(msg.size()%64!=56) msg.push_back(0);
    for(int i=7;i>=0;--i) msg.push_back((unsigned char)((bits>>(i*8))&0xFF));
    for(size_t i=0;i<msg.size();i+=64){
        uint32_t w[80];
        for(int j=0;j<16;++j)
            w[j]=((uint32_t)msg[i+j*4]<<24)|((uint32_t)msg[i+j*4+1]<<16)
               |((uint32_t)msg[i+j*4+2]<<8)|(uint32_t)msg[i+j*4+3];
        for(int j=16;j<80;++j) w[j]=sha1_rotl(w[j-3]^w[j-8]^w[j-14]^w[j-16],1);
        uint32_t a=h0,b=h1,c=h2,d=h3,e=h4;
        for(int j=0;j<80;++j){
            uint32_t f,k;
            if(j<20){f=(b&c)|((~b)&d);k=0x5A827999;}
            else if(j<40){f=b^c^d;k=0x6ED9EBA1;}
            else if(j<60){f=(b&c)|(b&d)|(c&d);k=0x8F1BBCDC;}
            else{f=b^c^d;k=0xCA62C1D6;}
            uint32_t t=sha1_rotl(a,5)+f+e+k+w[j];
            e=d;d=c;c=sha1_rotl(b,30);b=a;a=t;
        }
        h0+=a;h1+=b;h2+=c;h3+=d;h4+=e;
    }
    uint32_t hs[5]={h0,h1,h2,h3,h4};
    for(int i=0;i<5;++i){out[i*4]=(hs[i]>>24)&0xFF;out[i*4+1]=(hs[i]>>16)&0xFF;
        out[i*4+2]=(hs[i]>>8)&0xFF;out[i*4+3]=hs[i]&0xFF;}
}

static std::string b64_encode(const unsigned char* d,size_t l){
    static const char T[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;int v=0,b=-6;
    for(size_t i=0;i<l;++i){v=(v<<8)+d[i];b+=8;
        while(b>=0){o.push_back(T[(v>>b)&0x3F]);b-=6;}}
    if(b>-6) o.push_back(T[((v<<8)>>(b+8))&0x3F]);
    while(o.size()%4) o.push_back('=');
    return o;
}

static std::string ws_accept_key(const std::string& key){
    static const char* GUID="258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string s=key+GUID;unsigned char dig[20];
    sha1_hash((const unsigned char*)s.data(),s.size(),dig);
    return b64_encode(dig,20);
}

// ==================== HTTP Tools ====================
static std::string url_decode(const std::string& s){
    std::string o;
    for(size_t i=0;i<s.size();++i){
        if(s[i]=='%'&&i+2<s.size()){
            auto hx=[](char c)->int{
                if(c>='0'&&c<='9')return c-'0';
                if(c>='a'&&c<='f')return c-'a'+10;
                if(c>='A'&&c<='F')return c-'A'+10;
                return -1;};
            int h=hx(s[i+1]),l=hx(s[i+2]);
            if(h>=0&&l>=0){o.push_back((char)(h*16+l));i+=2;continue;}}
        o.push_back(s[i]);}
    return o;}

static std::string url_encode(const std::string& s){
    static const char* hex="0123456789ABCDEF";
    std::string o;
    for(unsigned char c:s){
        if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~'||c=='/')
            o.push_back((char)c);
        else { o.push_back('%'); o.push_back(hex[c>>4]); o.push_back(hex[c&15]); }
    }
    return o;}

static size_t parse_content_length(const std::string& head){
    std::string lo;lo.reserve(head.size());
    for(char c:head) lo.push_back(tolower((unsigned char)c));
    size_t p=lo.find("content-length:");
    if(p==std::string::npos) return 0;
    size_t i=p+15,v=0;
    while(i<lo.size()&&lo[i]==' ')++i;
    while(i<lo.size()&&isdigit((unsigned char)lo[i])){v=v*10+(lo[i]-'0');++i;}
    return v;}

static std::string read_file_str(const std::string& path){
    std::ifstream f(path,std::ios::binary);
    if(!f) return "";
    f.seekg(0,std::ios::end);auto sz=f.tellg();
    if(sz<=0) return "";
    std::string s(sz,'\0');f.seekg(0);f.read(&s[0],sz);return s;}
// ==================== 账号系统工具（SHA-256 + 加盐） ====================
static std::string sha256_hex(const std::string& msg){
    static const uint32_t K[64]={
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t h0=0x6a09e667,h1=0xbb67ae85,h2=0x3c6ef372,h3=0xa54ff53a,
             h4=0x510e527f,h5=0x9b05688c,h6=0x1f83d9ab,h7=0x5be0cd19;
    std::vector<uint8_t> data(msg.begin(),msg.end());
    uint64_t bitlen=(uint64_t)data.size()*8;
    data.push_back(0x80);
    while(data.size()%64!=56)data.push_back(0);
    for(int i=7;i>=0;--i)data.push_back((uint8_t)(bitlen>>(i*8)));
    auto rotr=[](uint32_t x,int n){return (x>>n)|(x<<(32-n));};
    for(size_t off=0;off<data.size();off+=64){
        uint32_t w[64];
        for(int i=0;i<16;i++)w[i]=((uint32_t)data[off+i*4]<<24)|((uint32_t)data[off+i*4+1]<<16)|((uint32_t)data[off+i*4+2]<<8)|((uint32_t)data[off+i*4+3]);
        for(int i=16;i<64;i++){
            uint32_t s0=rotr(w[i-15],7)^rotr(w[i-15],18)^(w[i-15]>>3);
            uint32_t s1=rotr(w[i-2],17)^rotr(w[i-2],19)^(w[i-2]>>10);
            w[i]=w[i-16]+s0+w[i-7]+s1;}
        uint32_t a=h0,b=h1,c=h2,d=h3,e=h4,f=h5,g=h6,hh=h7;
        for(int i=0;i<64;i++){
            uint32_t S1=rotr(e,6)^rotr(e,11)^rotr(e,25);
            uint32_t ch=(e&f)^((~e)&g);
            uint32_t t1=hh+S1+ch+K[i]+w[i];
            uint32_t S0=rotr(a,2)^rotr(a,13)^rotr(a,22);
            uint32_t maj=(a&b)^(a&c)^(b&c);
            uint32_t t2=S0+maj;
            hh=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;}
        h0+=a;h1+=b;h2+=c;h3+=d;h4+=e;h5+=f;h6+=g;h7+=hh;}
    char buf[65];snprintf(buf,sizeof(buf),"%08x%08x%08x%08x%08x%08x%08x%08x",h0,h1,h2,h3,h4,h5,h6,h7);
    return std::string(buf);}

static std::map<std::string,std::string> parse_form(const std::string& b){
    std::map<std::string,std::string> m;
    std::string cur;
    for(size_t i=0;i<=b.size();++i){
        if(i==b.size()||b[i]=='&'){
            size_t eq=cur.find('=');
            if(eq!=std::string::npos)m[url_decode(cur.substr(0,eq))]=url_decode(cur.substr(eq+1));
            cur.clear();}
        else cur.push_back(b[i]);}
    return m;}

static std::string gen_hex(int nbytes){
    static bool seeded=false;
    if(!seeded){srand((unsigned)(time(nullptr)^(getpid()<<16)));seeded=true;}
    char buf[128];int n=0;
    for(int i=0;i<nbytes;i++){int r=rand()%256;n+=snprintf(buf+n,sizeof(buf)-n,"%02x",r);}
    return std::string(buf,n);}

static std::string gen_session_token(){
    static std::atomic<unsigned long long> seq{0};
    unsigned long long s=seq.fetch_add(1);
    char buf[96];snprintf(buf,sizeof(buf),"%llx-%llx-%ld",
        (unsigned long long)time(nullptr),s,(long)getpid());
    return sha256_hex(std::string(buf)).substr(0,48);}

static std::string file_ext(const std::string& p){
    size_t d=p.rfind('.');if(d==std::string::npos) return "";
    std::string e=p.substr(d);for(auto&c:e) c=tolower((unsigned char)c);return e;}

static const char* mime_type(const std::string& p){
    std::string e=file_ext(p);
    if(e==".html"||e==".htm")return"text/html; charset=utf-8";
    if(e==".css")return"text/css; charset=utf-8";
    if(e==".js")return"application/javascript; charset=utf-8";
    if(e==".png")return"image/png";
    if(e==".jpg"||e==".jpeg")return"image/jpeg";
    if(e==".gif")return"image/gif";
    if(e==".webp")return"image/webp";
    if(e==".svg")return"image/svg+xml";
    if(e==".ico")return"image/x-icon";
    if(e==".json")return"application/json";
    if(e==".txt")return"text/plain; charset=utf-8";
    if(e==".mp4")return"video/mp4";
    if(e==".webm")return"video/webm";
    if(e==".ogg")return"video/ogg";
    if(e==".mov")return"video/quicktime";
    if(e==".avi")return"video/x-msvideo";
    if(e==".mkv")return"video/x-matroska";
    if(e==".mp3")return"audio/mpeg";
    if(e==".wav")return"audio/wav";
    if(e==".m4a")return"audio/mp4";
    if(e==".aac")return"audio/aac";
    return"application/octet-stream";}

static std::string err_page(int code,const char* txt){
    char b[256];
    int n=snprintf(b,sizeof(b),
        "<html><head><title>%d %s</title></head>"
        "<body style=\"font-family:sans-serif;text-align:center;padding:3rem\">"
        "<h1>%d %s</h1></body></html>\n",code,txt,code,txt);
    return std::string(b,n);}

// ==================== WebSocket Frame ====================
static int ws_frame(const unsigned char* buf,size_t len,uint8_t& op,std::string& payload){
    if(len<2) return 0;
    uint8_t b0=buf[0],b1=buf[1];op=b0&0x0F;
    bool masked=b1&0x80;uint64_t plen=b1&0x7F;size_t hd=2;
    if(plen==126){if(len<4)return 0;plen=((uint64_t)buf[2]<<8)|buf[3];hd=4;}
    else if(plen==127){if(len<10)return 0;plen=0;
        for(int i=0;i<8;++i)plen=(plen<<8)|buf[2+i];hd=10;}
    if(plen>MAX_FRAME)return -1;
    unsigned char mk[4]={};
    if(masked){if(len<hd+4)return 0;memcpy(mk,buf+hd,4);hd+=4;}
    if(len<hd+plen)return 0;
    payload.assign((const char*)buf+hd,(size_t)plen);
    if(masked)for(size_t i=0;i<plen;++i)payload[i]^=mk[i%4];
    return(int)(hd+plen);}

// ==================== Reactor ====================
class ChatUser;
class Handler{
public:
    int fd=-1;
    virtual~Handler()=default;
    virtual void on_read(){}
    virtual void on_write(){};
};

class ChatUser:public Handler{
public:
    char nick[64]={};
    std::string token;          // 持久身份标识（用于断线重连恢复房间）
    time_t last_active=time(nullptr);
    virtual bool is_chat(){return true;}
    virtual bool is_ws(){return false;}
    virtual void send_msg(const std::string&)=0;
    virtual void send_ping(){};
};

// ========== 多线程广播 ==========
struct BroadcastMsg {
    int mid;
    std::string text;
};
static std::queue<BroadcastMsg> g_bcast_q;
static std::mutex g_bcast_mtx;
static std::condition_variable g_bcast_cv;
static std::mutex g_hs_mtx; // 保护连接列表遍历

// ========== Redis 客户端封装 ==========
class RedisClient {
private:
    redisContext* ctx = nullptr;
    bool connected = false;
public:
    bool is_connected() const { return connected; }
    bool connect(const char* host = "127.0.0.1", int port = 6379) {
        ctx = redisConnect(host, port);
        if (ctx == nullptr || ctx->err) {
            std::cerr << "[Redis] 连接失败: " << (ctx ? ctx->errstr : "nullptr") << std::endl;
            connected = false;
            return false;
        }
        connected = true;
        std::cout << "[Redis] 连接成功" << std::endl;
        return true;
    }
    ~RedisClient() { if (ctx) redisFree(ctx); }
    redisReply* command(const char* fmt, ...) {
        if (!connected) return nullptr;
        va_list ap; va_start(ap, fmt);
        redisReply* r = (redisReply*)redisvCommand(ctx, fmt, ap);
        va_end(ap);
        if (r == nullptr) { std::cerr << "[Redis] 命令失败: " << ctx->errstr << std::endl; }
        return r;
    }
    // 在线用户
    void user_online(const std::string& nick) {
        redisReply* r = command("SADD chat:online_users %s", nick.c_str());
        if (r) freeReplyObject(r);
    }
    void user_offline(const std::string& nick) {
        redisReply* r = command("SREM chat:online_users %s", nick.c_str());
        if (r) freeReplyObject(r);
    }
    std::vector<std::string> get_online_users() {
        std::vector<std::string> users;
        redisReply* r = command("SMEMBERS chat:online_users");
        if (r && r->type == REDIS_REPLY_ARRAY) {
            for (size_t i = 0; i < r->elements; i++) users.push_back(r->element[i]->str);
        }
        if (r) freeReplyObject(r);
        return users;
    }
    int get_online_count() {
        int cnt = 0;
        redisReply* r = command("SCARD chat:online_users");
        if (r && r->type == REDIS_REPLY_INTEGER) cnt = (int)r->integer;
        if (r) freeReplyObject(r);
        return cnt;
    }
    // 消息缓存
    void add_message(const std::string& msg) {
        redisReply* r = command("LPUSH chat:messages %s", msg.c_str());
        if (r) freeReplyObject(r);
        r = command("LTRIM chat:messages 0 99");
        if (r) freeReplyObject(r);
    }
    // 撤回：按完整消息文本从 Redis 缓存精确删除（LREM 删除所有匹配项）
    void remove_message(const std::string& msg) {
        redisReply* r = command("LREM chat:messages 0 %s", msg.c_str());
        if (r) freeReplyObject(r);
    }
    std::vector<std::string> get_recent_messages(int count = 100) {
        std::vector<std::string> msgs;
        redisReply* r = command("LRANGE chat:messages 0 %d", count - 1);
        if (r && r->type == REDIS_REPLY_ARRAY) {
            for (int i = (int)r->elements - 1; i >= 0; i--) msgs.push_back(r->element[i]->str);
        }
        if (r) freeReplyObject(r);
        return msgs;
    }
    // 头像
    void set_avatar(const std::string& nick, const std::string& url) {
        redisReply* r = command("HSET chat:avatars %s %s", nick.c_str(), url.c_str());
        if (r) freeReplyObject(r);
    }
    std::string get_avatar(const std::string& nick) {
        std::string url;
        redisReply* r = command("HGET chat:avatars %s", nick.c_str());
        if (r && r->type == REDIS_REPLY_STRING) url = r->str;
        if (r) freeReplyObject(r);
        return url;
    }
    void del_avatar(const std::string& nick) {
        redisReply* r = command("HDEL chat:avatars %s", nick.c_str());
        if (r) freeReplyObject(r);
    }
};
static RedisClient g_redis;

// ========== SQLite 持久化客户端 ==========
class DatabaseClient {
private:
    sqlite3* db = nullptr;
    bool connected = false;
    bool exec(const char* sql) {
        char* err = nullptr;
        int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
        if (rc != SQLITE_OK) {
            std::cerr << "[SQLite] 执行失败: " << (err ? err : "unknown") << std::endl;
            if (err) sqlite3_free(err);
            return false;
        }
        return true;
    }
public:
    bool is_connected() const { return connected; }
    bool connect(const std::string& db_path = "chatroom.db") {
        int rc = sqlite3_open(db_path.c_str(), &db);
        if (rc != SQLITE_OK) {
            std::cerr << "[SQLite] 打开失败: " << sqlite3_errmsg(db) << std::endl;
            return false;
        }
        connected = true;
        std::cout << "[SQLite] 连接成功: " << db_path << std::endl;
        exec("CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY KEY AUTOINCREMENT, nick TEXT UNIQUE NOT NULL, avatar TEXT DEFAULT '', created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP, last_seen TIMESTAMP DEFAULT CURRENT_TIMESTAMP)");
        exec("CREATE TABLE IF NOT EXISTS messages (id INTEGER PRIMARY KEY AUTOINCREMENT, nick TEXT NOT NULL, content TEXT NOT NULL, msg_type TEXT DEFAULT 'text', created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP)");
        exec("CREATE TABLE IF NOT EXISTS avatars (nick TEXT PRIMARY KEY, url TEXT NOT NULL, updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP)");
        exec("CREATE TABLE IF NOT EXISTS posts (id INTEGER PRIMARY KEY AUTOINCREMENT, username TEXT NOT NULL, content TEXT DEFAULT '', images TEXT DEFAULT '', privacy INTEGER DEFAULT 0, created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP)");
        exec("CREATE TABLE IF NOT EXISTS favorites (id INTEGER PRIMARY KEY AUTOINCREMENT, username TEXT NOT NULL, msg_id INTEGER DEFAULT 0, sender TEXT DEFAULT '', content TEXT DEFAULT '', msg_time INTEGER DEFAULT 0, created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP)");
        {sqlite3_stmt* ch; bool has_priv=false;
        if(sqlite3_prepare_v2(db,"PRAGMA table_info(posts)",-1,&ch,nullptr)==SQLITE_OK){
            while(sqlite3_step(ch)==SQLITE_ROW){const char* v=(const char*)sqlite3_column_text(ch,1);if(v&&strcmp(v,"privacy")==0)has_priv=true;}
            sqlite3_finalize(ch);}
        if(!has_priv)exec("ALTER TABLE posts ADD COLUMN privacy INTEGER DEFAULT 0");}
        {sqlite3_stmt* ch2; bool has_mt=false;
        if(sqlite3_prepare_v2(db,"PRAGMA table_info(favorites)",-1,&ch2,nullptr)==SQLITE_OK){
            while(sqlite3_step(ch2)==SQLITE_ROW){const char* v=(const char*)sqlite3_column_text(ch2,1);if(v&&strcmp(v,"msg_time")==0)has_mt=true;}
            sqlite3_finalize(ch2);}
        if(!has_mt)exec("ALTER TABLE favorites ADD COLUMN msg_time INTEGER DEFAULT 0");}
        ensure_account_columns();
        ensure_admin();
        return true;
    }
    ~DatabaseClient() { if (db) sqlite3_close(db); }
    void user_join(const std::string& nick) {
        sqlite3_stmt* stmt;
        const char* sql = "INSERT INTO users (nick) VALUES (?) ON CONFLICT(nick) DO UPDATE SET last_seen=CURRENT_TIMESTAMP";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, nick.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt); sqlite3_finalize(stmt);
        }
    }
    void add_message(const std::string& nick, const std::string& content, const std::string& type = "text") {
        sqlite3_stmt* stmt;
        const char* sql = "INSERT INTO messages (nick, content, msg_type) VALUES (?, ?, ?)";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, nick.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, content.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, type.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt); sqlite3_finalize(stmt);
        }
    }
    std::vector<std::pair<std::string, std::string>> get_recent_messages(int limit = 100) {
        // 返回 (unix时间戳, 昵称：内容)，供历史消息带发送时间
        std::vector<std::pair<std::string, std::string>> msgs;
        sqlite3_stmt* stmt;
        const char* sql = "SELECT strftime('%s', created_at) AS t, nick, content FROM (SELECT created_at, nick, content FROM messages ORDER BY id DESC LIMIT ?) ORDER BY id ASC";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, limit);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                std::string t = (const char*)sqlite3_column_text(stmt, 0);
                std::string nick = (const char*)sqlite3_column_text(stmt, 1);
                std::string content = (const char*)sqlite3_column_text(stmt, 2);
                msgs.push_back({t, nick+"："+content});
            }
            sqlite3_finalize(stmt);
        }
        return msgs;
    }
    void set_avatar(const std::string& nick, const std::string& url) {
        sqlite3_stmt* stmt;
        const char* sql = "INSERT INTO avatars (nick, url) VALUES (?, ?) ON CONFLICT(nick) DO UPDATE SET url=?, updated_at=CURRENT_TIMESTAMP";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, nick.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, url.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, url.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt); sqlite3_finalize(stmt);
        }
        sqlite3_stmt* stmt2;
        const char* sql2 = "UPDATE users SET avatar=? WHERE nick=?";
        if (sqlite3_prepare_v2(db, sql2, -1, &stmt2, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt2, 1, url.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt2, 2, nick.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt2); sqlite3_finalize(stmt2);
        }
    }
    void del_avatar(const std::string& nick) {
        sqlite3_stmt* stmt;
        const char* sql = "DELETE FROM avatars WHERE nick=?";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, nick.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt); sqlite3_finalize(stmt);
        }
    }
    std::map<std::string, std::string> get_all_avatars() {
        std::map<std::string, std::string> avs;
        sqlite3_stmt* stmt;
        const char* sql = "SELECT nick, url FROM avatars";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                std::string nick = (const char*)sqlite3_column_text(stmt, 0);
                std::string url = (const char*)sqlite3_column_text(stmt, 1);
                avs[nick] = url;
            }
            sqlite3_finalize(stmt);
        }
        return avs;
    }
    // ---- 账号系统 ----
    void ensure_account_columns(){
        bool has_u=false,has_p=false,has_s=false,has_t=false,has_a=false,has_b=false;
        sqlite3_stmt* st;
        if(sqlite3_prepare_v2(db,"PRAGMA table_info(users)",-1,&st,nullptr)==SQLITE_OK){
            while(sqlite3_step(st)==SQLITE_ROW){
                const char* nm=(const char*)sqlite3_column_text(st,1);
                if(!nm)continue;std::string n=nm;
                if(n=="username")has_u=true;if(n=="pass_hash")has_p=true;
                if(n=="salt")has_s=true;if(n=="token")has_t=true;
                if(n=="is_admin")has_a=true;if(n=="banned")has_b=true;}
            sqlite3_finalize(st);}
        if(!has_u)exec("ALTER TABLE users ADD COLUMN username TEXT DEFAULT ''");
        if(!has_p)exec("ALTER TABLE users ADD COLUMN pass_hash TEXT DEFAULT ''");
        if(!has_s)exec("ALTER TABLE users ADD COLUMN salt TEXT DEFAULT ''");
        if(!has_t)exec("ALTER TABLE users ADD COLUMN token TEXT DEFAULT ''");
        if(!has_a)exec("ALTER TABLE users ADD COLUMN is_admin INTEGER DEFAULT 0");
        if(!has_b)exec("ALTER TABLE users ADD COLUMN banned INTEGER DEFAULT 0");
    }
    // 返回 0=成功,1=用户名已存在,2=失败；成功时 token_out 为会话 token
    int register_user(const std::string& u,const std::string& p,std::string& token_out){
        if(u.empty()||p.empty())return 2;
        sqlite3_stmt* st;
        if(sqlite3_prepare_v2(db,"SELECT id FROM users WHERE username=?",-1,&st,nullptr)==SQLITE_OK){
            sqlite3_bind_text(st,1,u.c_str(),-1,SQLITE_TRANSIENT);
            bool exists=sqlite3_step(st)==SQLITE_ROW;
            sqlite3_finalize(st);
            if(exists)return 1;}
        std::string salt=gen_hex(16);
        std::string ph=sha256_hex(salt+p);
        std::string tk=gen_session_token();
        if(sqlite3_prepare_v2(db,"INSERT INTO users (nick,username,pass_hash,salt,token,created_at) VALUES (?,?,?,?,?,datetime('now','localtime'))",-1,&st,nullptr)==SQLITE_OK){
            sqlite3_bind_text(st,1,u.c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_bind_text(st,2,u.c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_bind_text(st,3,ph.c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_bind_text(st,4,salt.c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_bind_text(st,5,tk.c_str(),-1,SQLITE_TRANSIENT);
            bool ok=sqlite3_step(st)==SQLITE_DONE;
            sqlite3_finalize(st);
            if(!ok)return 2;
            token_out=tk;return 0;
        }
        return 2;
    }
    // 验证登录，成功返回新会话 token，失败返回空串
    std::string login_user(const std::string& u,const std::string& p){
        std::string salt,ph;
        sqlite3_stmt* st;
        if(sqlite3_prepare_v2(db,"SELECT salt,pass_hash FROM users WHERE username=?",-1,&st,nullptr)==SQLITE_OK){
            sqlite3_bind_text(st,1,u.c_str(),-1,SQLITE_TRANSIENT);
            if(sqlite3_step(st)==SQLITE_ROW){
                const char* s=(const char*)sqlite3_column_text(st,0);
                const char* h=(const char*)sqlite3_column_text(st,1);
                if(s)salt=s;if(h)ph=h;}
            sqlite3_finalize(st);}
        if(salt.empty()||ph.empty())return "";
        if(sha256_hex(salt+p)!=ph)return "";
        std::string newtk=gen_session_token();
        if(sqlite3_prepare_v2(db,"UPDATE users SET token=?, last_seen=CURRENT_TIMESTAMP WHERE username=?",-1,&st,nullptr)==SQLITE_OK){
            sqlite3_bind_text(st,1,newtk.c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_bind_text(st,2,u.c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_step(st);sqlite3_finalize(st);}
        return newtk;
    }
    // 根据会话 token 找昵称（用于自动登录）
    std::string nick_by_token(const std::string& tk){
        if(tk.empty())return "";
        std::string nick;
        sqlite3_stmt* st;
        if(sqlite3_prepare_v2(db,"SELECT nick FROM users WHERE token=?",-1,&st,nullptr)==SQLITE_OK){
            sqlite3_bind_text(st,1,tk.c_str(),-1,SQLITE_TRANSIENT);
            if(sqlite3_step(st)==SQLITE_ROW){const char* n=(const char*)sqlite3_column_text(st,0);if(n)nick=n;}
            sqlite3_finalize(st);}
        return nick;
    }
    // ---- 管理员 / 封禁 ----
    void ensure_admin(){
        sqlite3_stmt* st;bool has=false;
        if(sqlite3_prepare_v2(db,"SELECT id FROM users WHERE is_admin=1 LIMIT 1",-1,&st,nullptr)==SQLITE_OK){
            has=sqlite3_step(st)==SQLITE_ROW;sqlite3_finalize(st);}
        if(has)return;
        std::string salt=gen_hex(16);
        std::string ph=sha256_hex(salt+std::string("admin123"));
        if(sqlite3_prepare_v2(db,"INSERT INTO users (nick,username,pass_hash,salt,token,is_admin) VALUES ('admin','admin',?,?,?,1)",-1,&st,nullptr)==SQLITE_OK){
            sqlite3_bind_text(st,1,ph.c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_bind_text(st,2,salt.c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_bind_text(st,3,"",-1,SQLITE_TRANSIENT);
            sqlite3_step(st);sqlite3_finalize(st);
            std::cout<<"[账号] 已创建默认管理员: admin / admin123 （请尽快登录修改密码）"<<std::endl;}
    }
    int user_is_admin(const std::string& nick){
        if(nick.empty())return 0;
        sqlite3_stmt* st;int ret=0;
        if(sqlite3_prepare_v2(db,"SELECT is_admin FROM users WHERE username=?",-1,&st,nullptr)==SQLITE_OK){
            sqlite3_bind_text(st,1,nick.c_str(),-1,SQLITE_TRANSIENT);
            if(sqlite3_step(st)==SQLITE_ROW)ret=sqlite3_column_int(st,0);
            sqlite3_finalize(st);}
        return ret;
    }
    void set_ban(const std::string& nick,int val){
        sqlite3_stmt* st;
        if(sqlite3_prepare_v2(db,"UPDATE users SET banned=? WHERE username=?",-1,&st,nullptr)==SQLITE_OK){
            sqlite3_bind_int(st,1,val);sqlite3_bind_text(st,2,nick.c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_step(st);sqlite3_finalize(st);}
    }
    int is_banned(const std::string& nick){
        if(nick.empty())return 0;
        sqlite3_stmt* st;int ret=0;
        if(sqlite3_prepare_v2(db,"SELECT banned FROM users WHERE username=?",-1,&st,nullptr)==SQLITE_OK){
            sqlite3_bind_text(st,1,nick.c_str(),-1,SQLITE_TRANSIENT);
            if(sqlite3_step(st)==SQLITE_ROW)ret=sqlite3_column_int(st,0);
            sqlite3_finalize(st);}
        return ret;
    }
    std::vector<std::string> list_users(){
        std::vector<std::string> out;
        sqlite3_stmt* st;
        if(sqlite3_prepare_v2(db,"SELECT username,is_admin,banned FROM users WHERE username!='' ORDER BY id",-1,&st,nullptr)==SQLITE_OK){
            while(sqlite3_step(st)==SQLITE_ROW){
                std::string u=(const char*)sqlite3_column_text(st,0);
                int adm=sqlite3_column_int(st,1),ban=sqlite3_column_int(st,2);
                out.push_back(u+"|"+(adm?"A":"U")+"|"+(ban?"B":"-"));
            }
            sqlite3_finalize(st);}
        return out;
    }
    // 通过显示昵称反查登录账号（改名后 nick 可能 != username）
    std::string get_username(const std::string& nick){
        std::string un;
        sqlite3_stmt* st;
        if(sqlite3_prepare_v2(db,"SELECT username FROM users WHERE nick=?",-1,&st,nullptr)==SQLITE_OK){
            sqlite3_bind_text(st,1,nick.c_str(),-1,SQLITE_TRANSIENT);
            if(sqlite3_step(st)==SQLITE_ROW){const char* v=(const char*)sqlite3_column_text(st,0);if(v)un=v;}
            sqlite3_finalize(st);}
        return un;
    }
    int verify_pwd(const std::string& nick,const std::string& p){
        std::string salt,ph;
        sqlite3_stmt* st;
        if(sqlite3_prepare_v2(db,"SELECT salt,pass_hash FROM users WHERE username=?",-1,&st,nullptr)==SQLITE_OK){
            sqlite3_bind_text(st,1,nick.c_str(),-1,SQLITE_TRANSIENT);
            if(sqlite3_step(st)==SQLITE_ROW){
                const char* s=(const char*)sqlite3_column_text(st,0);
                const char* h=(const char*)sqlite3_column_text(st,1);
                if(s)salt=s;if(h)ph=h;}
            sqlite3_finalize(st);}
        if(salt.empty()||ph.empty())return 0;
        return sha256_hex(salt+p)==ph?1:0;
    }
    int change_pwd(const std::string& nick,const std::string& newpass){
        std::string salt=gen_hex(16);
        std::string ph=sha256_hex(salt+newpass);
        sqlite3_stmt* st;
        if(sqlite3_prepare_v2(db,"UPDATE users SET pass_hash=?, salt=? WHERE username=?",-1,&st,nullptr)==SQLITE_OK){
            sqlite3_bind_text(st,1,ph.c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_bind_text(st,2,salt.c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_bind_text(st,3,nick.c_str(),-1,SQLITE_TRANSIENT);
            bool ok=sqlite3_step(st)==SQLITE_DONE;
            sqlite3_finalize(st);
            return ok?0:2;}
        return 2;
    }
    void clear_messages(){ exec("DELETE FROM messages"); }
    void remove_message(const std::string& nick, const std::string& content){
        sqlite3_stmt* st;
        if(sqlite3_prepare_v2(db,"DELETE FROM messages WHERE nick=? AND content=?",-1,&st,nullptr)==SQLITE_OK){
            sqlite3_bind_text(st,1,nick.c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_bind_text(st,2,content.c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_step(st);
            sqlite3_finalize(st);}
    }
    // 改昵称：同步更新 users 表显示昵称列（登录账号 username 不变）
    void rename_user(const std::string& old_nick, const std::string& new_nick){
        sqlite3_stmt* st;
        if(sqlite3_prepare_v2(db,"UPDATE users SET nick=? WHERE nick=?",-1,&st,nullptr)==SQLITE_OK){
            sqlite3_bind_text(st,1,new_nick.c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_bind_text(st,2,old_nick.c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_step(st);
            sqlite3_finalize(st);}
    }
    // 查询账号资料：返回 {username,nick,created_at,avatar}
    std::vector<std::string> get_profile(const std::string& nick){
        std::vector<std::string> r;
        sqlite3_stmt* st;
        if(sqlite3_prepare_v2(db,"SELECT username,nick,created_at,avatar FROM users WHERE nick=?",-1,&st,nullptr)==SQLITE_OK){
            sqlite3_bind_text(st,1,nick.c_str(),-1,SQLITE_TRANSIENT);
            if(sqlite3_step(st)==SQLITE_ROW){
                for(int i=0;i<4;i++){
                    const char* v=(const char*)sqlite3_column_text(st,i);
                    r.push_back(v?v:"");}
            }
            sqlite3_finalize(st);}
        return r;
    }
    // ===== 朋友圈（个人动态） =====
    long long add_post(const std::string& username, const std::string& content, const std::string& images, int privacy=0){
        sqlite3_stmt* st;
        long long id=0;
        if(sqlite3_prepare_v2(db,"INSERT INTO posts (username,content,images,privacy) VALUES (?,?,?,?)",-1,&st,nullptr)==SQLITE_OK){
            sqlite3_bind_text(st,1,username.c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_bind_text(st,2,content.c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_bind_text(st,3,images.c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_bind_int(st,4,privacy);
            sqlite3_step(st); sqlite3_finalize(st);
            id=sqlite3_last_insert_rowid(db);}
        return id;
    }
    // 返回动态列表：id|nick|avatar|content|images|time|privacy;  （content/images 已 URL 编码）
    // viewer_uname：查看者账号；时间线只返回【公开 + 查看者自己的】动态
    std::string get_posts(int offset, int limit, const std::string& viewer_uname){
        std::string out;
        sqlite3_stmt* st;
        if(sqlite3_prepare_v2(db,
            "SELECT p.id,u.nick,u.avatar,p.content,p.images,p.privacy,p.created_at "
            "FROM posts p LEFT JOIN users u ON p.username=u.username "
            "WHERE p.privacy=0 OR p.username=? "
            "ORDER BY p.id DESC LIMIT ? OFFSET ?",-1,&st,nullptr)==SQLITE_OK){
            sqlite3_bind_text(st,1,viewer_uname.c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_bind_int(st,2,limit); sqlite3_bind_int(st,3,offset);
            while(sqlite3_step(st)==SQLITE_ROW){
                long long id=sqlite3_column_int64(st,0);
                std::string nick=sqlite3_column_text(st,1)?(const char*)sqlite3_column_text(st,1):"";
                std::string avatar=sqlite3_column_text(st,2)?(const char*)sqlite3_column_text(st,2):"";
                std::string content=sqlite3_column_text(st,3)?(const char*)sqlite3_column_text(st,3):"";
                std::string images=sqlite3_column_text(st,4)?(const char*)sqlite3_column_text(st,4):"";
                int privacy=sqlite3_column_int(st,5);
                std::string ctime=sqlite3_column_text(st,6)?(const char*)sqlite3_column_text(st,6):"";
                out+=std::to_string(id)+"|"+url_encode(nick)+"|"+url_encode(avatar)+"|"
                    +url_encode(content)+"|"+url_encode(images)+"|"+url_encode(ctime)+"|"
                    +std::to_string(privacy)+";";}
            sqlite3_finalize(st);}
        return out;
    }
    // 查看某人主页动态：viewer 看自己=全部，看别人=仅公开。返回格式同 get_posts
    std::string get_user_posts(const std::string& nick, const std::string& viewer_uname, int offset, int limit){
        std::string out;
        std::string target_uname;
        {sqlite3_stmt* s2;
        if(sqlite3_prepare_v2(db,"SELECT username FROM users WHERE nick=?",-1,&s2,nullptr)==SQLITE_OK){
            sqlite3_bind_text(s2,1,nick.c_str(),-1,SQLITE_TRANSIENT);
            if(sqlite3_step(s2)==SQLITE_ROW){const char* v=(const char*)sqlite3_column_text(s2,0);if(v)target_uname=v;}
            sqlite3_finalize(s2);} }
        bool is_self=(!target_uname.empty()&&target_uname==viewer_uname);
        std::string sql=is_self
            ?"SELECT p.id,u.nick,u.avatar,p.content,p.images,p.privacy,p.created_at FROM posts p LEFT JOIN users u ON p.username=u.username WHERE u.nick=? ORDER BY p.id DESC LIMIT ? OFFSET ?"
            :"SELECT p.id,u.nick,u.avatar,p.content,p.images,p.privacy,p.created_at FROM posts p LEFT JOIN users u ON p.username=u.username WHERE u.nick=? AND p.privacy=0 ORDER BY p.id DESC LIMIT ? OFFSET ?";
        sqlite3_stmt* st;
        if(sqlite3_prepare_v2(db,sql.c_str(),-1,&st,nullptr)==SQLITE_OK){
            sqlite3_bind_text(st,1,nick.c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_bind_int(st,2,limit); sqlite3_bind_int(st,3,offset);
            while(sqlite3_step(st)==SQLITE_ROW){
                long long id=sqlite3_column_int64(st,0);
                std::string pn=sqlite3_column_text(st,1)?(const char*)sqlite3_column_text(st,1):"";
                std::string avatar=sqlite3_column_text(st,2)?(const char*)sqlite3_column_text(st,2):"";
                std::string content=sqlite3_column_text(st,3)?(const char*)sqlite3_column_text(st,3):"";
                std::string images=sqlite3_column_text(st,4)?(const char*)sqlite3_column_text(st,4):"";
                int privacy=sqlite3_column_int(st,5);
                std::string ctime=sqlite3_column_text(st,6)?(const char*)sqlite3_column_text(st,6):"";
                out+=std::to_string(id)+"|"+url_encode(pn)+"|"+url_encode(avatar)+"|"
                    +url_encode(content)+"|"+url_encode(images)+"|"+url_encode(ctime)+"|"
                    +std::to_string(privacy)+";";}
            sqlite3_finalize(st);}
        return out;
    }
    // 删除自己的动态（校验 username 防止删别人）
    bool del_post(long long id, const std::string& username){
        sqlite3_stmt* st;
        bool ok=false;
        if(sqlite3_prepare_v2(db,"DELETE FROM posts WHERE id=? AND username=?",-1,&st,nullptr)==SQLITE_OK){
            sqlite3_bind_int64(st,1,id);
            sqlite3_bind_text(st,2,username.c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_step(st);
            ok=sqlite3_changes(db)>0;
            sqlite3_finalize(st);}
        return ok;
    }
    // 修改某条动态的权限（校验归属：只能改自己的）
    bool set_post_priv(long long id, const std::string& username, int priv){
        sqlite3_stmt* st;
        bool ok=false;
        if(sqlite3_prepare_v2(db,"UPDATE posts SET privacy=? WHERE id=? AND username=?",-1,&st,nullptr)==SQLITE_OK){
            sqlite3_bind_int(st,1,priv);
            sqlite3_bind_int64(st,2,id);
            sqlite3_bind_text(st,3,username.c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_step(st);
            ok=sqlite3_changes(db)>0;
            sqlite3_finalize(st);}
        return ok;
    }
    // ===== 消息收藏 =====
    // 收藏一条消息（内容存副本，聊天记录清理后依然保留）；已收藏同一 msg_id 返回 0
    long long add_favorite(const std::string& username, long long msg_id, const std::string& sender, const std::string& content, long long msg_time){
        sqlite3_stmt* st;
        long long id=0;
        if(sqlite3_prepare_v2(db,"SELECT id FROM favorites WHERE username=? AND msg_id=?",-1,&st,nullptr)==SQLITE_OK){
            sqlite3_bind_text(st,1,username.c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_bind_int64(st,2,msg_id);
            if(sqlite3_step(st)==SQLITE_ROW) id=sqlite3_column_int64(st,0);
            sqlite3_finalize(st);}
        if(id) return 0;
        if(sqlite3_prepare_v2(db,"INSERT INTO favorites (username,msg_id,sender,content,msg_time) VALUES (?,?,?,?,?)",-1,&st,nullptr)==SQLITE_OK){
            sqlite3_bind_text(st,1,username.c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_bind_int64(st,2,msg_id);
            sqlite3_bind_text(st,3,sender.c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_bind_text(st,4,content.c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_bind_int64(st,5,msg_time);
            if(sqlite3_step(st)==SQLITE_DONE) id=sqlite3_last_insert_rowid(db);
            sqlite3_finalize(st);}
        return id;
    }
    // 我的收藏列表：fav_id|sender|content|time;  （content 做 url_encode 防 | 与 ; 冲突）
    std::string get_favorites(const std::string& username, int offset, int limit){
        std::string out;
        sqlite3_stmt* st;
        if(sqlite3_prepare_v2(db,"SELECT id,sender,content,COALESCE(msg_time, strftime('%s', created_at)) FROM favorites WHERE username=? ORDER BY id DESC LIMIT ? OFFSET ?",-1,&st,nullptr)==SQLITE_OK){
            sqlite3_bind_text(st,1,username.c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_bind_int(st,2,limit);
            sqlite3_bind_int(st,3,offset);
            while(sqlite3_step(st)==SQLITE_ROW){
                long long fid=sqlite3_column_int64(st,0);
                const char* sn=(const char*)sqlite3_column_text(st,1);
                const char* ct=(const char*)sqlite3_column_text(st,2);
                const char* tm=(const char*)sqlite3_column_text(st,3);
                out+=std::to_string(fid)+"|"+url_encode(sn?sn:"")+"|"+url_encode(ct?ct:"")+"|"+url_encode(tm?tm:"")+";";
            }
            sqlite3_finalize(st);}
        return out;
    }
    // 取消收藏（校验 username 防删别人）
    bool del_favorite(long long id, const std::string& username){
        sqlite3_stmt* st;
        bool ok=false;
        if(sqlite3_prepare_v2(db,"DELETE FROM favorites WHERE id=? AND username=?",-1,&st,nullptr)==SQLITE_OK){
            sqlite3_bind_int64(st,1,id);
            sqlite3_bind_text(st,2,username.c_str(),-1,SQLITE_TRANSIENT);
            sqlite3_step(st);
            ok=sqlite3_changes(db)>0;
            sqlite3_finalize(st);}
        return ok;
    }
};
static DatabaseClient g_db;

// ========== 中国象棋引擎（服务端权威） ==========
static xq::Engine g_chess;                 // 服务器端的棋盘（所有游戏命令共享）
static std::mutex g_chess_mtx;             // 保护引擎
static std::vector<xq::Engine> g_chess_hist; // 悔棋快照

// ========== 联机对战房间管理 ==========
struct GameRoom{
    std::string code;
    ChatUser* red=nullptr;    // 红方
    ChatUser* black=nullptr;  // 黑方
    xq::Engine eng;           // 本房间独立的引擎
    bool started=false;
    int total_time=0;         // 对局总时长（秒，0=不限）
    int step_time=0;          // 每步限时（秒，0=不限）
    int red_left=0, black_left=0;   // 双方剩余总时间
    time_t red_step_start=0, black_step_start=0;  // 当前步开始时刻
    time_t last_broadcast=0;  // 上次广播时间防刷屏
    std::string red_token, black_token;   // 双方 token（座位占用标识，非空=有主）
    time_t red_offline=0, black_offline=0;  // 断线时刻（0=在线）
    bool red_ready=false, black_ready=false;  // 双方是否已准备
    std::vector<xq::Engine> hist;   // 历史局面（联机悔棋用，makeMove 前快照）
    std::vector<xq::Side> movers;   // 每步的走子方（用于精确判断悔棋步数）
    bool undo_pending=false;        // 是否有待处理的悔棋请求
    xq::Side undo_who=xq::RED;      // 悔棋请求方
};
static std::map<std::string,GameRoom> g_rooms;
static std::map<ChatUser*,std::string> g_user_room;
static std::mutex g_room_mtx;
static std::atomic<int> g_room_seq{1000};

// ==================== 五子棋（Gomoku）房间 ====================
struct G5Room{
    std::string code;
    ChatUser* b=nullptr;   // 黑（先手）
    ChatUser* w=nullptr;   // 白（后手）
    std::string b_token,w_token;
    time_t b_offline=0,w_offline=0;
    bool b_ready=false,w_ready=false;
    char g[15][15]={{0}};  // 0空 1黑 2白
    int turn=1;            // 1黑 2白
    bool started=false,over=false;
    int winner=0;          // 0无 1黑胜 2白胜
    std::vector<std::pair<int,int>> hist;  // 落子历史 (x,y)
    std::vector<int> mv_side;             // 每步落子方（1黑 2白，悔棋判定用）
    bool undo_pending=false; int undo_who=0;
};
static std::map<std::string,G5Room> g5_rooms;
static std::map<ChatUser*,std::string> g5_user_room;

// 五子棋判胜：以 (x,y) 为中心检查四个方向是否五连
static bool g5_check(char g[15][15],int x,int y,int side){
    const int dx[4]={1,0,1,1};
    const int dy[4]={0,1,1,-1};
    for(int d=0;d<4;d++){
        int cnt=1;
        for(int s=1;s<5;s++){int nx=x+dx[d]*s,ny=y+dy[d]*s;if(nx<0||nx>=15||ny<0||ny>=15||g[ny][nx]!=side)break;cnt++;}
        for(int s=1;s<5;s++){int nx=x-dx[d]*s,ny=y-dy[d]*s;if(nx<0||nx>=15||ny<0||ny>=15||g[ny][nx]!=side)break;cnt++;}
        if(cnt>=5)return true;
    }
    return false;
}
static std::string g5_side_name(int s){return s==1?"黑":"白";}
static std::mutex g5_mtx;
static void g5_broadcast(G5Room&r,const std::string&msg){
    if(r.b)r.b->send_msg(msg);
    if(r.w)r.w->send_msg(msg);
}
static void g5_leave(ChatUser*u){
    auto it=g5_user_room.find(u);
    if(it==g5_user_room.end())return;
    std::string code=it->second;
    auto rit=g5_rooms.find(code);
    if(rit!=g5_rooms.end()){
        G5Room&r=rit->second;
        ChatUser*other=(r.b==u)?r.w:r.b;
        if(other){other->send_msg("G5|peer_left\n");g5_user_room.erase(other);}
        g5_rooms.erase(rit);
    }
    g5_user_room.erase(it);
}
// ==================== 贪吃蛇双人（Snake PVP）房间 ====================
struct SnkRoom{
    std::string code;
    ChatUser* a=nullptr;
    ChatUser* b=nullptr;
    bool ra=false, rb=false;   // 双方准备状态
    int food_count=1;          // 食物个数（创建房间可设置）
    long speed_ms=160;         // 移动速度 ms/格（创建房间可设置）
    snk::PVPEngine g;
};
static std::map<std::string,SnkRoom> snk_rooms;
static std::map<ChatUser*,std::string> snk_user_room;
static std::atomic<int> snk_room_seq{2000};
static std::string snk_room_code(){
    for(int t=0;t<20;t++){
        int n=snk_room_seq.fetch_add(1,std::memory_order_relaxed);
        char b[8];snprintf(b,sizeof(b),"S%04d",n%10000);
        if(snk_rooms.count(b)==0)return std::string(b);
    }
    return "S0000";
}
// ==================== 坦克大战（TN|） ====================
struct TnRoom{
    std::string code;
    ChatUser* a=nullptr;
    ChatUser* b=nullptr;
    bool ra=false, rb=false;
    int gsize=24, lives=3;
    tn::PVPEngine g;
};
static std::map<std::string,TnRoom> tn_rooms;
static std::map<ChatUser*,std::string> tn_user_room;
static std::atomic<int> tn_room_seq{3000};
static std::string tn_room_code(){
    for(int i=0;i<100;i++){
        int n=++tn_room_seq;
        char b[8];snprintf(b,sizeof(b),"T%04d",n%10000);
        if(tn_rooms.count(b)==0)return std::string(b);
    }
    return "T0001";
}
static void tn_leave(ChatUser*u){
    auto it=tn_user_room.find(u);
    if(it==tn_user_room.end())return;
    std::string code=it->second;
    auto rit=tn_rooms.find(code);
    if(rit!=tn_rooms.end()){
        TnRoom&r=rit->second;
        ChatUser* other=(r.a==u)?r.b:r.a;
        if(other){other->send_msg("TN|peer_left\n");tn_user_room.erase(other);}
        tn_rooms.erase(rit);
    }
    tn_user_room.erase(it);
}
static void snk_leave(ChatUser*u){
    auto it=snk_user_room.find(u);
    if(it==snk_user_room.end())return;
    std::string code=it->second;
    auto rit=snk_rooms.find(code);
    if(rit!=snk_rooms.end()){
        SnkRoom&r=rit->second;
        ChatUser*other=(r.a==u)?r.b:r.a;
        if(other){other->send_msg("SNK|peer_left\n");snk_user_room.erase(other);}
        snk_rooms.erase(rit);
    }
    snk_user_room.erase(it);
}
// ---- 枪战肉鸽联机房间 ----
struct GunRoom{
    std::string code;
    ChatUser* a=nullptr;   // 炮台 0（左）
    ChatUser* b=nullptr;   // 炮台 1（右）
    bool ra=false, rb=false;
    int diff=1;
    gun::Engine g;
    bool running=false;
    long long lastActive=0;   // 最近活动时间（房间保留/超时清理用）
};
static std::map<std::string,GunRoom> gun_rooms;
static std::map<ChatUser*,std::string> gun_user_room;
static std::atomic<int> gun_room_seq{4000};
static std::string gun_room_code(){
    for(int i=0;i<100;i++){
        int n=++gun_room_seq;
        char b[8];snprintf(b,sizeof(b),"G%04d",n%10000);
        if(gun_rooms.count(b)==0)return std::string(b);
    }
    return "G0001";
}
static long long snk_now_ms();   // 前向声明（定义在下方）
static void gun_leave(ChatUser*u){
    auto it=gun_user_room.find(u);
    if(it==gun_user_room.end())return;
    std::string code=it->second;
    auto rit=gun_rooms.find(code);
    if(rit!=gun_rooms.end()){
        GunRoom&r=rit->second;
        bool wasA=(r.a==u), wasB=(r.b==u);
        ChatUser* other=(wasA?r.b:r.a);
        if(wasA){r.a=nullptr; r.ra=false;}   // 离开者准备状态一并重置
        if(wasB){r.b=nullptr; r.rb=false;}
        if(r.running){
            // 对局中离开 → 解散房间并通知对方
            if(other)other->send_msg("GUN|peer_left\n");
            gun_rooms.erase(rit);
        }else{
            // 未开局 → 房间保留（即使全空也等新人补位，链接仍有效），靠超时清理
            r.lastActive=snk_now_ms();
            if(other)other->send_msg("GUN|peer_wait\n");
        }
    }
    gun_user_room.erase(it);
}
static int gun_pi(ChatUser*u){
    auto it=gun_user_room.find(u);
    if(it==gun_user_room.end())return -1;
    auto rit=gun_rooms.find(it->second);
    if(rit==gun_rooms.end())return -1;
    if(rit->second.a==u)return 0;
    if(rit->second.b==u)return 1;
    return -1;
}
static std::string gen_room_code(){
    for(int tries=0;tries<20;tries++){
        int n=g_room_seq.fetch_add(1,std::memory_order_relaxed);
        char b[8];snprintf(b,sizeof(b),"%04d",n%10000);
        std::string code(b);
        if(g_rooms.count(code)==0)return code;
    }
    return "0000";
}
static void room_broadcast(GameRoom&r,const std::string&m){
    if(r.red)r.red->send_msg(m);
    if(r.black)r.black->send_msg(m);
}
static void leave_room(ChatUser*u){
    auto it=g_user_room.find(u);
    if(it==g_user_room.end())return;
    std::string code=it->second;
    auto rit=g_rooms.find(code);
    if(rit!=g_rooms.end()){
        GameRoom&r=rit->second;
        ChatUser*other=(r.red==u)?r.black:r.red;
        if(other){
            other->send_msg("GAME|peer_left\n");
            g_user_room.erase(other);   // 对方也立即移出房间，回到单机
        }
        g_rooms.erase(rit);
    }
    g_user_room.erase(it);
}

// 用户断线：如果在房间，保留座位并通知对方等待重连（60秒窗口）
static void on_user_disconnect(ChatUser*u){
    auto it=g_user_room.find(u);
    if(it==g_user_room.end())return;   // 不在房间
    std::string code=it->second;
    auto rit=g_rooms.find(code);
    if(rit==g_rooms.end())return;
    GameRoom&r=rit->second;
    time_t now=time(nullptr);
    if(r.red==u){
        r.red=nullptr;r.red_offline=now;
        if(r.black)r.black->send_msg("GAME|peer_offline\n");
    }else if(r.black==u){
        r.black=nullptr;r.black_offline=now;
        if(r.red)r.red->send_msg("GAME|peer_offline\n");
    }
    g_user_room.erase(it);   // 用户移出 user_room 映射，但房间保留待恢复
    // ---- 五子棋断线处理 ----
    {
        std::lock_guard<std::mutex> lk(g5_mtx);
        auto it5=g5_user_room.find(u);
        if(it5!=g5_user_room.end()){
            std::string c5=it5->second;
            auto rit5=g5_rooms.find(c5);
            if(rit5!=g5_rooms.end()){
                G5Room&r=rit5->second;
                if(r.b==u){r.b=nullptr;r.b_offline=now;if(r.w)r.w->send_msg("G5|peer_offline\n");}
                else if(r.w==u){r.w=nullptr;r.w_offline=now;if(r.b)r.b->send_msg("G5|peer_offline\n");}
            }
            g5_user_room.erase(it5);
        }
    }
}

// 检查房间超时（在 reactor 主循环中周期调用）
static void check_room_timeouts(){
    std::lock_guard<std::mutex> lk(g_room_mtx);
    if(g_rooms.empty())return;
    time_t now=time(nullptr);
    std::vector<std::string>to_del;
    for(auto&kv:g_rooms){
        GameRoom&r=kv.second;
        if(!r.started)continue;
        // 离线超过60秒，清房（对方回单机）
        if(r.red_offline>0&&now-r.red_offline>60){
            if(r.black)r.black->send_msg("GAME|peer_left\n");
            g_user_room.erase(r.black);
            to_del.push_back(kv.first);continue;
        }
        if(r.black_offline>0&&now-r.black_offline>60){
            if(r.red)r.red->send_msg("GAME|peer_left\n");
            g_user_room.erase(r.red);
            to_del.push_back(kv.first);continue;
        }
        // 有人离线时暂停计时与超时判定
        if(r.red_offline>0||r.black_offline>0)continue;
        // 总时间耗尽
        if(r.total_time>0&&r.red_left<=0){
            room_broadcast(r,"GAME|timeout|r\n");
            to_del.push_back(kv.first);continue;
        }
        if(r.total_time>0&&r.black_left<=0){
            room_broadcast(r,"GAME|timeout|b\n");
            to_del.push_back(kv.first);continue;
        }
        // 每步限时：当前轮到谁
        if(r.step_time>0){
            bool red_turn=r.eng.turn==xq::RED;
            time_t step_start=red_turn?r.red_step_start:r.black_step_start;
            int used=(int)(now-step_start);
            if(used>r.step_time){
                room_broadcast(r,std::string("GAME|timeout|")+(red_turn?"r":"b")+"\n");
                to_del.push_back(kv.first);continue;
            }
        }
        // 定期广播时间（每10秒，让前端校准）—— 广播"有效剩余"（扣掉当前步已用时间），
        // 否则服务器 red_left 只在走子时才扣，前端本地倒计时会被周期性地重置跳回
        if(r.total_time>0&&(now-r.last_broadcast>=10)){
            int eff_red=r.red_left, eff_black=r.black_left;
            if(r.red_offline==0&&r.black_offline==0){
                if(r.eng.turn==xq::RED){
                    int used=(int)(now-r.red_step_start);
                    if(r.step_time>0&&used>r.step_time)used=r.step_time;
                    eff_red=r.red_left-used;if(eff_red<0)eff_red=0;
                }else{
                    int used=(int)(now-r.black_step_start);
                    if(r.step_time>0&&used>r.step_time)used=r.step_time;
                    eff_black=r.black_left-used;if(eff_black<0)eff_black=0;
                }
            }
            room_broadcast(r,"GAME|time|"+std::to_string(eff_red)+"|"+std::to_string(eff_black)+"\n");
            r.last_broadcast=now;
        }
    }
    for(auto&code:to_del){
        auto rit=g_rooms.find(code);
        if(rit==g_rooms.end())continue;
        std::string cd=code;
        // 让双方移出房间
        if(rit->second.red){g_user_room.erase(rit->second.red);}
        if(rit->second.black){g_user_room.erase(rit->second.black);}
        g_rooms.erase(rit);
        // 双方重新建? 不，直接单机
        (void)cd;
    }

    // ---- 五子棋房间：离线超60秒清房 ----
    {
        std::lock_guard<std::mutex> lk(g5_mtx);
        std::vector<std::string> g5_del;
        for(auto&kv:g5_rooms){
            G5Room&r=kv.second;
            if(r.b_offline>0&&now-r.b_offline>60){
                if(r.w)r.w->send_msg("G5|peer_left\n");
                if(r.w)g5_user_room.erase(r.w);
                g5_del.push_back(kv.first);continue;
            }
            if(r.w_offline>0&&now-r.w_offline>60){
                if(r.b)r.b->send_msg("G5|peer_left\n");
                if(r.b)g5_user_room.erase(r.b);
                g5_del.push_back(kv.first);continue;
            }
        }
        for(auto&cd2:g5_del){auto rit5=g5_rooms.find(cd2);if(rit5!=g5_rooms.end())g5_rooms.erase(rit5);}
    }
}

// 贪吃蛇：毫秒时间戳
static long long snk_now_ms(){
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

class Reactor{
public:
    int ep=-1;
    epoll_event evs[MAX_EVENTS];
    std::vector<std::unique_ptr<Handler>>hs;
    std::vector<Handler*>dead;
    int conn_count=0;
    time_t last_public_msg_time=0;

    bool init(){ep=epoll_create1(0);return ep>=0;}
    ~Reactor(){if(ep>=0)close(ep);}
    bool reg(Handler*h,uint32_t m){epoll_event e{};e.data.ptr=h;e.events=m;return epoll_ctl(ep,EPOLL_CTL_ADD,h->fd,&e)==0;}
    bool mod(Handler*h,uint32_t m){
        if(h->fd<0)return false;
        epoll_event e{};e.data.ptr=h;e.events=m;
        if(epoll_ctl(ep,EPOLL_CTL_MOD,h->fd,&e)<0){if(errno==ENOENT)return true;return false;}
        return true;}
    void kill(Handler*h){
        if(h->fd>=0){epoll_ctl(ep,EPOLL_CTL_DEL,h->fd,nullptr);close(h->fd);h->fd=-1;conn_count--;}
        for(auto*d:dead)if(d==h)return;
        dead.push_back(h);}
    void sweep(){
        for(auto*h:dead){
            // 用户下线，从 Redis 移除
            auto*u=dynamic_cast<ChatUser*>(h);
            if(u&&u->nick[0]!='\0'){
                g_redis.user_offline(u->nick);
                auto sit=sess_map.find(u->nick);
                if(sit!=sess_map.end()&&sit->second==u)sess_map.erase(sit);
                t3_games.erase(u);
                snk_games.erase(u);
                tn_games.erase(u);
                gun_games.erase(u);
                {
                    // 坦克大战：断线保留房间（角色置空等待重连），两人都走才删
                    auto tru=tn_user_room.find(u);
                    if(tru!=tn_user_room.end()){
                        std::string tcode=tru->second;
                        auto trit=tn_rooms.find(tcode);
                        if(trit!=tn_rooms.end()){
                            TnRoom&tr=trit->second;
                            if(tr.a==u)tr.a=nullptr;
                            if(tr.b==u)tr.b=nullptr;
                            tn_user_room.erase(tru);
                            ChatUser* tother=tr.a?tr.a:tr.b;
                            if(tother)tother->send_msg("TN|peer_left\n");
                            if(!tr.a&&!tr.b)tn_rooms.erase(trit);
                        }
                    }
                }
                {
                    auto sru=snk_user_room.find(u);
                    if(sru!=snk_user_room.end()){
                        std::string scode=sru->second;
                        auto srit=snk_rooms.find(scode);
                        if(srit!=snk_rooms.end()){
                            ChatUser* other=(srit->second.a==u)?srit->second.b:srit->second.a;
                            if(other){other->send_msg("SNK|peer_left\n");snk_user_room.erase(other);}
                            snk_rooms.erase(srit);
                        }
                        snk_user_room.erase(sru);
                    }
                }
                {
                    if(gun_user_room.count(u))gun_leave(u);   // 断线：未开局保留房间，对局中解散
                }
            }
            if(u){std::lock_guard<std::mutex> rl(g_room_mtx);on_user_disconnect(u);}
            {std::lock_guard<std::mutex> hl(g_hs_mtx);
             hs.erase(std::remove_if(hs.begin(),hs.end(),[&](auto&p){return p.get()==h;}),hs.end());}
        }
        dead.clear();}
    void run(){
        while(g_running){
            int n=epoll_wait(ep,evs,MAX_EVENTS,50);
            if(n<0){if(errno==EINTR)continue;break;}
            for(int i=0;i<n;++i){
                auto*h=(Handler*)evs[i].data.ptr;
                uint32_t ev=evs[i].events;
                if(ev&(EPOLLERR|EPOLLHUP)){
                    h->on_read();
                    if(h->fd>=0)kill(h);
                    continue;}
                if(ev&EPOLLIN){h->on_read();if(h->fd<0)continue;}
                if(h->fd>=0&&(evs[i].events&EPOLLOUT))h->on_write();}
            sweep();
            check_room_timeouts();
            snake_tick_all();
            tank_tick_all();
            gun_tick_all();}}

    void broadcast(const std::string&s);
    void handle_msg(ChatUser*self,const std::string&text);
    void snake_tick_all();
    void tank_tick_all();
    void gun_tick_all();
    int online_count();
    ChatUser*find_user(const std::string&name);

    static const size_t HIST_MAX=100;static const long HIST_TTL=86400;
    std::deque<std::pair<time_t,std::string>>hist;
    std::string hist_file;
    void load_hist(const std::string&p);
    void save_hist(const std::string&msg);
    void send_hist(ChatUser*u);
    void purge_hist();
    void rewrite_hist();

    std::map<std::string,std::string>avatars;
    std::string avt_file;
    void load_avatars(const std::string&p);
    void save_avatars();
    void send_avatars(ChatUser*u);
    void broadcast_avatar(const std::string&name,const std::string&url);
    void ping_all();
    // 撤回功能
    std::map<std::string,ChatUser*> sess_map; // 单点登录：账号 -> 当前在线连接
    std::map<ChatUser*,tmg::GameState> t3_games; // 三消：每个连接一个对局
    std::map<ChatUser*,snk::Engine> snk_games;  // 贪吃蛇：每个连接一个对局
    std::map<ChatUser*,tn::Engine> tn_games;    // 坦克大战：每个连接一个对局
    std::map<ChatUser*,gun::Engine> gun_games;  // 枪战肉鸽：每个连接一个对局
    std::map<int,std::pair<std::string,std::string>> msg_index; // id -> {nick, text}
    std::deque<int> recent_msg_ids;
    int next_msg_id=1;
    int record_msg(const std::string& nick,const std::string& text);
    void recall_last(ChatUser* self);
    void recall_by_id(ChatUser* self,int mid);
    void broadcast_msg(int mid,const std::string& text);
    void do_broadcast_batch();};

// ==================== 聊天记录 ====================
void Reactor::load_hist(const std::string&p){
    hist_file=p;std::ifstream f(p);
    if(!f)return;
    time_t cut=time(nullptr)-HIST_TTL;std::string line;
    while(std::getline(f,line)){
        if(line.empty())continue;
        time_t ts=time(nullptr);std::string msg=line;
        size_t tab=line.find('\t');
        if(tab!=std::string::npos&&tab>0&&tab<=12){
            bool dig=true;
            for(size_t i=0;i<tab;++i)if(!isdigit((unsigned char)line[i])){dig=false;break;}
            if(dig){ts=(time_t)strtoll(line.substr(0,tab).c_str(),nullptr,10);msg=line.substr(tab+1);}}
        if(msg.empty()||ts<cut)continue;
        hist.push_back({ts,msg+"\n"});
        if(hist.size()>HIST_MAX)hist.pop_front();}
    std::cout<<"Loaded "<<hist.size()<<" history messages"<<std::endl;}

void Reactor::save_hist(const std::string&msg){
    time_t now=time(nullptr);
    hist.push_back({now,msg});
    if(hist.size()>HIST_MAX)hist.pop_front();
    std::ofstream f(hist_file,std::ios::app);
    if(f)f<<now<<'\t'<<msg;
    // 同步写入 Redis 缓存
    g_redis.add_message(msg);
    // 同步写入 SQLite 持久化
    size_t colon=msg.find("：");
    if(colon!=std::string::npos){
        std::string nick=msg.substr(0,colon);
        std::string content=msg.substr(colon+1);
        std::string type="text";
        if(content.find("[图片]")==0)type="image";
        else if(content.find("[文件]")==0)type="file";
        else if(content.find("[视频]")==0)type="video";
        else if(content.find("[语音]")==0)type="audio";
        g_db.add_message(nick,content,type);
    }}

void Reactor::rewrite_hist(){
    if(hist_file.empty())return;
    std::ofstream f(hist_file,std::ios::trunc);
    for(auto&h:hist)f<<h.first<<'\t'<<h.second;}

void Reactor::purge_hist(){
    if(hist.empty())return;
    time_t cut=time(nullptr)-HIST_TTL;size_t before=hist.size();
    while(!hist.empty()&&hist.front().first<cut)hist.pop_front();
    if(hist.size()!=before)rewrite_hist();}

void Reactor::send_hist(ChatUser*u){
    // 历史消息统一分配 MSGID（供前端收藏/定位），并带消息发送时间戳
    auto send_one=[&](const std::string&text, const std::string&tsec="0"){
        std::string nick; size_t cp=text.find_first_of("：:");
        if(cp!=std::string::npos&&cp>0) nick=text.substr(0,cp);
        int mid=record_msg(nick,text);
        u->send_msg("MSGID|"+std::to_string(mid)+"|"+tsec+"|"+text);
    };
    // 优先从 Redis 缓存读取
    if(g_redis.is_connected()){
        auto msgs = g_redis.get_recent_messages(100);
        if(!msgs.empty()){
            u->send_msg("--- recent "+std::to_string(msgs.size())+" messages ---\n");
            for(auto&m:msgs)send_one(m);
            u->send_msg("--- end of history ---\n");
            return;
        }
    }
    // 其次从 SQLite 持久化读取
    if(g_db.is_connected()){
        auto msgs = g_db.get_recent_messages(100);
        if(!msgs.empty()){
            u->send_msg("--- recent "+std::to_string(msgs.size())+" messages ---\n");
            for(auto&m:msgs)send_one(m.second, m.first);
            u->send_msg("--- end of history ---\n");
            return;
        }
    }
    // 最后降级到内存
    if(hist.empty())return;
    u->send_msg("--- recent "+std::to_string(hist.size())+" messages ---\n");
    for(auto&m:hist)send_one(m.second, std::to_string((long long)m.first));
    u->send_msg("--- end of history ---\n");}

// ==================== 头像 ====================
void Reactor::load_avatars(const std::string&p){
    avt_file=p;std::ifstream f(p);
    if(!f)return;
    std::string line;
    while(std::getline(f,line)){
        size_t t=line.find('\t');
        if(t==std::string::npos)continue;
        std::string n=line.substr(0,t),u=line.substr(t+1);
        while(!u.empty()&&(u.back()=='\r'||u.back()=='\n'))u.pop_back();
        if(!n.empty()&&!u.empty())avatars[n]=u;}
    std::cout<<"Loaded "<<avatars.size()<<" avatars"<<std::endl;}

void Reactor::save_avatars(){
    if(avt_file.empty())return;
    std::ofstream f(avt_file,std::ios::trunc);
    for(auto&kv:avatars)f<<kv.first<<'\t'<<kv.second<<'\n';}

void Reactor::send_avatars(ChatUser*u){
    if(!u->is_ws())return;
    for(auto&kv:avatars)u->send_msg("AVATAR|"+kv.first+"|"+kv.second+"\n");}

void Reactor::broadcast_avatar(const std::string&name,const std::string&url){
    std::string proto="AVATAR|"+name+"|"+url+"\n";
    std::string human="【系统】"+name+(url.empty()?" 恢复了默认头像\n":" 更新了头像\n");
    for(auto&c:hs){auto*u=(ChatUser*)c.get();if(u->is_chat()&&u->fd>=0)u->send_msg(u->is_ws()?proto:human);}}

// ==================== 聊天逻辑 ====================
void Reactor::broadcast(const std::string&s){
    for(auto&c:hs){auto*u=(ChatUser*)c.get();if(u->is_chat()&&u->fd>=0)u->send_msg(s);}}
int Reactor::record_msg(const std::string&nick,const std::string&text){
    int id=next_msg_id++;
    msg_index[id]={nick,text};
    recent_msg_ids.push_back(id);
    if(recent_msg_ids.size()>200){int old=recent_msg_ids.front();recent_msg_ids.pop_front();msg_index.erase(old);}
    return id;}
void Reactor::broadcast_msg(int mid,const std::string&text){
    // 时间分隔检查（在主线程）
    time_t now=time(nullptr);
    if(last_public_msg_time>0&&now-last_public_msg_time>=600){
        struct tm*lt=localtime(&now);
        char buf[32];strftime(buf,sizeof(buf),"%Y-%m-%d %H:%M",lt);
        std::string tmsg=std::string("TIME|")+buf+"\n";
        // 时间分隔也放入广播队列
        {std::lock_guard<std::mutex> lk(g_bcast_mtx);g_bcast_q.push({-1,tmsg});}
        save_hist(tmsg);
    }
    last_public_msg_time=now;
    // 把消息放入广播队列，由广播线程异步发送（不阻塞主线程）
    {std::lock_guard<std::mutex> lk(g_bcast_mtx);g_bcast_q.push({mid,text});}
    g_bcast_cv.notify_one();
}

// 实际广播发送（在广播线程执行，批量处理队列中的消息）
void Reactor::do_broadcast_batch(){
    std::vector<BroadcastMsg> msgs;
    {std::unique_lock<std::mutex> lk(g_bcast_mtx);
    g_bcast_cv.wait(lk,[]{return !g_bcast_q.empty()||!g_running;});
    if(!g_running&&g_bcast_q.empty())return;
    while(!g_bcast_q.empty()){msgs.push_back(g_bcast_q.front());g_bcast_q.pop();}}
    if(msgs.empty())return;
    // 加锁保护连接列表，遍历一次连接，把所有消息批量发给每个连接
    std::lock_guard<std::mutex> hs_lk(g_hs_mtx);
    for(auto&c:hs){
        auto*u=(ChatUser*)c.get();
        if(u->is_chat()&&u->fd>=0){
            for(auto&m:msgs){
                if(m.mid<0){
                    // 时间分隔消息，不带MSGID
                    u->send_msg(m.text);
                }else if(u->is_ws()){
                    u->send_msg("MSGID|"+std::to_string(m.mid)+"|"+std::to_string((long long)time(nullptr))+"|"+m.text);
                }else{
                    u->send_msg(m.text);
                }
            }
        }
    }
}
void Reactor::recall_last(ChatUser*self){
    int found_id=-1;
    for(auto it=recent_msg_ids.rbegin();it!=recent_msg_ids.rend();++it){
        auto mit=msg_index.find(*it);
        if(mit!=msg_index.end()&&mit->second.first==self->nick){found_id=*it;break;}}
    if(found_id<0){self->send_msg("【系统】没有可撤回的消息\n");return;}
    std::string full_text=msg_index[found_id].second;
    msg_index.erase(found_id);
    recent_msg_ids.erase(std::remove(recent_msg_ids.begin(),recent_msg_ids.end(),found_id),recent_msg_ids.end());
    for(auto hit=hist.begin();hit!=hist.end();++hit){if(hit->second==full_text){hist.erase(hit);break;}}
    rewrite_hist();
    // 同步删除 Redis 缓存与 SQLite 持久化中的该条消息
    g_redis.remove_message(full_text);
    { size_t cl=full_text.find_first_of("：:"); if(cl!=std::string::npos&&cl>0){ g_db.remove_message(full_text.substr(0,cl), full_text.substr(cl+1)); } }
    std::string proto="RECALL|"+std::to_string(found_id)+"|"+std::string(self->nick)+"\n";
    std::string human="【系统】"+std::string(self->nick)+" 撤回了一条消息\n";
    for(auto&c:hs){
        auto*u=(ChatUser*)c.get();
        if(u->is_chat()&&u->fd>=0) u->send_msg(u->is_ws()?proto:human);}
    std::cout<<"[RECALL] id="<<found_id<<" nick="<<self->nick<<std::endl;}
void Reactor::recall_by_id(ChatUser* self,int mid){
    auto it=msg_index.find(mid);
    if(it==msg_index.end()){self->send_msg("【系统】消息不存在或已撤回\n");return;}
    if(it->second.first!=std::string(self->nick)){self->send_msg("【系统】只能撤回自己的消息\n");return;}
    std::string full_text=it->second.second;
    msg_index.erase(it);
    recent_msg_ids.erase(std::remove(recent_msg_ids.begin(),recent_msg_ids.end(),mid),recent_msg_ids.end());
    for(auto hit=hist.begin();hit!=hist.end();++hit){if(hit->second==full_text){hist.erase(hit);break;}}
    rewrite_hist();
    // 同步删除 Redis 缓存与 SQLite 持久化中的该条消息，避免重进聊天室再次出现
    g_redis.remove_message(full_text);
    { size_t cl=full_text.find_first_of("：:"); if(cl!=std::string::npos&&cl>0){ g_db.remove_message(full_text.substr(0,cl), full_text.substr(cl+1)); } }
    std::string proto="RECALL|"+std::to_string(mid)+"|"+std::string(self->nick)+"\n";
    std::string human="【系统】"+std::string(self->nick)+" 撤回了一条消息\n";
    for(auto&c:hs){
        auto*u=(ChatUser*)c.get();
        if(u->is_chat()&&u->fd>=0) u->send_msg(u->is_ws()?proto:human);}
    std::cout<<"[RECALL] id="<<mid<<" nick="<<self->nick<<std::endl;}

int Reactor::online_count(){
    int n=0;for(auto&c:hs){auto*u=(ChatUser*)c.get();if(u->is_chat()&&u->fd>=0)++n;}return n;}

ChatUser*Reactor::find_user(const std::string&name){
    for(auto&c:hs){auto*u=(ChatUser*)c.get();if(u->is_chat()&&u->fd>=0&&name==u->nick)return u;}
    return nullptr;}

void Reactor::snake_tick_all(){
    long long now_ms=snk_now_ms();
    // 单机贪吃蛇
    for(auto&kv:snk_games){
        snk::Engine&g=kv.second;
        if(!g.running || g.paused)continue;
        if(now_ms - g.last_tick < g.speed_ms)continue;
        g.last_tick=now_ms;
        int rc=g.tick();
        if(rc==1){
            kv.first->send_msg("SNK|over|"+std::to_string(g.score)+"\n");
        }else{
            kv.first->send_msg("SNK|state|"+g.dump()+"|"
                +std::to_string(g.food.x)+","+std::to_string(g.food.y)+"|"
                +std::to_string(g.score)+"\n");
        }
    }
    // 双人贪吃蛇房间
    for(auto&kv:snk_rooms){
        SnkRoom&r=kv.second;
        if(!r.a || !r.b)continue;
        snk::PVPEngine&g=r.g;
        if(!g.running || g.paused)continue;
        if(now_ms - g.last_tick < g.speed_ms)continue;
        g.last_tick=now_ms;
        int rc=g.tick();
        std::string st="SNK|pstate|"+g.dumpA()+"|"+g.dumpB()+"|"
            +g.dumpFoods()+"|"
            +std::to_string(g.scoreA)+"|"+std::to_string(g.scoreB)+"\n";
        r.a->send_msg(st);
        r.b->send_msg(st);
        if(rc==1){
            std::string res="SNK|pover|"+std::to_string(g.result)+"\n";
            r.a->send_msg(res);
            r.b->send_msg(res);
        }
    }
}

void Reactor::tank_tick_all(){
    long long now_ms=snk_now_ms();
    // 单机
    for(auto&kv:tn_games){
        tn::Engine&g=kv.second;
        if(!g.running)continue;
        if(now_ms-g.last_tick<40)continue;
        g.last_tick=now_ms;
        int rc=g.tick();
        if(rc==1){
            kv.first->send_msg(std::string("TN|over|")+(g.win?"win":"lose")+"|"+std::to_string(g.score)+"\n");
        }else{
            kv.first->send_msg("TN|state|"+g.dump_state()+"\n");
        }
    }
    // 双人
    for(auto&kv:tn_rooms){
        TnRoom&r=kv.second;
        if(!r.a||!r.b)continue;
        tn::PVPEngine&g=r.g;
        if(!g.running)continue;
        if(now_ms-g.last_tick<40)continue;
        g.last_tick=now_ms;
        int rc=g.tick();
        std::string st="TN|pstate|"+g.dump_state()+"\n";
        r.a->send_msg(st);
        r.b->send_msg(st);
        if(rc==1){
            std::string res="TN|pover|"+std::to_string(g.result)+"\n";
            r.a->send_msg(res);
            r.b->send_msg(res);
        }
    }
}

void Reactor::gun_tick_all(){
    long long now_ms=snk_now_ms();
    // 清理过期房间：未开局且超过 5 分钟无活动 → 删除（防泄漏）
    for(auto git=gun_rooms.begin();git!=gun_rooms.end();){
        GunRoom&gr=git->second;
        if(!gr.running && now_ms-gr.lastActive>300000){ git=gun_rooms.erase(git); }
        else ++git;
    }
    // 单机
    for(auto&kv:gun_games){
        gun::Engine&g=kv.second;
        if(g.over)continue;
        if(now_ms-g.last_tick<40)continue;
        g.last_tick=now_ms;
        int rc=g.tick();
        if(rc){
            kv.first->send_msg("GUN|over|"+std::to_string(g.result)+"\n");
        }else{
            kv.first->send_msg("GUN|state|"+g.dump_state()+"\n");
        }
    }
    // 联机房间：双人共守，tick 结果广播给双方
    for(auto&kv:gun_rooms){
        GunRoom&r=kv.second;
        if(!r.running||!r.a||!r.b)continue;
        gun::Engine&g=r.g;
        if(g.over)continue;
        if(now_ms-g.last_tick<40)continue;
        g.last_tick=now_ms;
        int rc=g.tick();
        if(rc){
            std::string ov="GUN|over|"+std::to_string(g.result)+"\n";
            if(r.a)r.a->send_msg(ov);
            if(r.b)r.b->send_msg(ov);
        }else{
            std::string st="GUN|state|"+g.dump_state()+"\n";
            if(r.a)r.a->send_msg(st);
            if(r.b)r.b->send_msg(st);
        }
    }
}

void Reactor::handle_msg(ChatUser*self,const std::string&text){
    if(text.empty())return;
    {std::string tr=text;while(!tr.empty()&&(tr.back()=='\n'||tr.back()=='\r'||tr.back()==' '||tr.back()=='\t'))tr.pop_back();
    if(tr=="ping"){self->send_msg("pong\n");return;}}

        // ========== 朋友圈（个人动态，POST 协议） ==========
    if(text.rfind("POST|",0)==0){
        std::string rest=text.substr(5);
        size_t p=rest.find('|');
        std::string content=(p==std::string::npos)?rest:rest.substr(0,p);
        int privacy=0;
        if(p!=std::string::npos)privacy=atoi(rest.c_str()+p+1);
        if(privacy!=1)privacy=0;
        std::string uname=g_db.get_username(self->nick);
        if(uname.empty()){self->send_msg("POST_ERR|请先登录\n");return;}
        long long id=g_db.add_post(uname,content,"",privacy);
        if(id)self->send_msg("POST_OK|"+std::to_string(id)+"\n");
        else self->send_msg("POST_ERR|发布失败\n");
        return;
    }
    if(text.rfind("POSTIMG|",0)==0){
        std::string rest=text.substr(8);
        size_t p=rest.find('|');
        std::string content=(p==std::string::npos)?rest:rest.substr(0,p);
        std::string imgs=""; int privacy=0;
        if(p!=std::string::npos){
            size_t q=rest.find('|',p+1);
            if(q==std::string::npos){imgs=rest.substr(p+1);}
            else{imgs=rest.substr(p+1,q-p-1);privacy=atoi(rest.c_str()+q+1);}
        }
        if(privacy!=1)privacy=0;
        std::string uname=g_db.get_username(self->nick);
        if(uname.empty()){self->send_msg("POST_ERR|请先登录\n");return;}
        long long id=g_db.add_post(uname,content,imgs,privacy);
        if(id)self->send_msg("POST_OK|"+std::to_string(id)+"\n");
        else self->send_msg("POST_ERR|发布失败\n");
        return;
    }
    if(text.rfind("POSTS|",0)==0){
        int offset=0,limit=20;
        const char* p=text.c_str()+6;
        offset=atoi(p);
        const char* q=strchr(p,'|'); if(q)limit=atoi(q+1);
        if(limit<1)limit=20; if(limit>50)limit=50;
        std::string uname=g_db.get_username(self->nick);
        std::string list=g_db.get_posts(offset,limit,uname);
        self->send_msg("POSTS|"+list+"\n");
        return;
    }
    if(text.rfind("POSTS_USER|",0)==0){
        std::string rest=text.substr(11);
        size_t p=rest.find('|');
        std::string tn=(p==std::string::npos)?rest:rest.substr(0,p);
        int offset=0,limit=20;
        if(p!=std::string::npos){
            offset=atoi(rest.c_str()+p+1);
            const char* q=strchr(rest.c_str()+p+1,'|'); if(q)limit=atoi(q+1);
        }
        if(limit<1)limit=20; if(limit>50)limit=50;
        std::string uname=g_db.get_username(self->nick);
        std::string list=g_db.get_user_posts(tn,uname,offset,limit);
        self->send_msg("POSTS_USER|"+list+"\n");
        return;
    }
    if(text.rfind("POSTDEL|",0)==0){
        long long id=atoll(text.c_str()+8);
        std::string uname=g_db.get_username(self->nick);
        g_db.del_post(id,uname);
        self->send_msg("POSTDEL_OK|"+std::to_string(id)+"\n");
        return;
    }
    if(text.rfind("POSTPRIV|",0)==0){
        // POSTPRIV|id|0/1 （0=公开 1=私密），只能改自己的动态
        const char* p=text.c_str()+9;
        long long id=atoll(p);
        const char* q=strchr(p,'|');
        int priv=0;
        if(q&&(q[1]=='1'))priv=1;
        std::string uname=g_db.get_username(self->nick);
        if(uname.empty()){self->send_msg("POSTPRIV_ERR|请先登录\n");return;}
        bool ok=g_db.set_post_priv(id,uname,priv);
        self->send_msg(ok?("POSTPRIV_OK|"+std::to_string(id)+"|"+std::to_string(priv)+"\n")
                       :("POSTPRIV_ERR|只能修改自己的动态\n"));
        return;
    }
    // ========== 消息收藏 ==========
    if(text.rfind("FAV|",0)==0){
        std::string rest=text.substr(4);
        size_t p=rest.find('|');
        long long mid=atoll(rest.c_str());
        std::string sender="",content=""; long long msg_time=0;
        if(p!=std::string::npos){
            size_t q=rest.find('|',p+1);
            sender=url_decode(rest.substr(p+1,q==std::string::npos?std::string::npos:q-p-1));
            if(q!=std::string::npos){
                size_t r=rest.find('|',q+1);
                std::string seg=rest.substr(q+1,r==std::string::npos?std::string::npos:r-q-1);
                bool dig=!seg.empty(); for(char c:seg) if(!isdigit((unsigned char)c)){dig=false;break;}
                if(dig){ msg_time=atoll(seg.c_str()); if(r!=std::string::npos) content=url_decode(rest.substr(r+1)); }
                else content=url_decode(rest.substr(q+1));
            }
        }
        std::string uname=g_db.get_username(self->nick);
        if(uname.empty()){self->send_msg("FAV_ERR|请先登录\n");return;}
        long long id=g_db.add_favorite(uname,mid,sender,content,msg_time);
        if(id) self->send_msg("FAV_OK|"+std::to_string(id)+"\n");
        else self->send_msg("FAV_ERR|已收藏过这条消息\n");
        return;
    }
    if(text.rfind("UNFAV|",0)==0){
        long long id=atoll(text.c_str()+6);
        std::string uname=g_db.get_username(self->nick);
        if(g_db.del_favorite(id,uname)) self->send_msg("UNFAV_OK|"+std::to_string(id)+"\n");
        else self->send_msg("UNFAV_ERR|取消失败\n");
        return;
    }
    if(text.rfind("FAVS|",0)==0){
        int offset=0,limit=20;
        const char* p=text.c_str()+5;
        offset=atoi(p);
        const char* q=strchr(p,'|'); if(q)limit=atoi(q+1);
        if(limit<1)limit=20; if(limit>50)limit=50;
        std::string uname=g_db.get_username(self->nick);
        std::string list=g_db.get_favorites(uname,offset,limit);
        self->send_msg("FAVS|"+list+"\n");
        return;
    }

        // ========== 贪吃蛇协议（SNK|） v1（服务端权威引擎定时 tick） ==========
    if(text.rfind("SNK|",0)==0){
        std::string cmd=text.substr(4);
        if(cmd.rfind("new",0)==0 || cmd=="restart"){
            snk::Engine&g=snk_games[self];
            g.init(snk_now_ms());
            self->send_msg("SNK|init|"+g.dump()+"|"
                +std::to_string(g.food.x)+","+std::to_string(g.food.y)+"|"
                +std::to_string(g.score)+"\n");
            return;
        }
        if(cmd.rfind("dir|",0)==0){
            const char* p=cmd.c_str()+4;
            int x=atoi(p), y=0;
            const char* q=strchr(p,'|');
            if(q)y=atoi(q+1);
            auto it=snk_games.find(self);
            if(it!=snk_games.end())it->second.set_dir(x,y);
            return;
        }
        if(cmd.rfind("pause|",0)==0){
            int pv=atoi(cmd.c_str()+6);
            auto it=snk_games.find(self);
            if(it!=snk_games.end())it->second.paused=(pv==1);
            return;
        }
        if(cmd.rfind("boost|",0)==0){
            int bv=atoi(cmd.c_str()+6);
            auto it=snk_games.find(self);
            if(it!=snk_games.end())it->second.boost=(bv==1);
            return;
        }
        // ---- 双人联机（创建/加入后等待双方准备） ----
        if(cmd.rfind("create",0)==0){
            if(snk_user_room.count(self)){self->send_msg("SNK|err|你已在房间里\n");return;}
            int fc=1; long spd=160;
            if(cmd.size()>7&&cmd[6]=='|'){
                const char* p=cmd.c_str()+7;
                fc=atoi(p);
                const char* q=strchr(p,'|'); if(q)spd=atol(q+1);
            }
            if(fc<1)fc=1; if(fc>8)fc=8;
            if(spd<70)spd=70; if(spd>500)spd=500;
            std::string code=snk_room_code();
            SnkRoom r;r.code=code;r.a=self;r.ra=false;r.rb=false;r.food_count=fc;r.speed_ms=spd;
            snk_rooms[code]=std::move(r);
            snk_user_room[self]=code;
            self->send_msg("SNK|created|"+code+"\n");
            return;
        }
        if(cmd.rfind("join|",0)==0){
            std::string code=cmd.substr(5);
            if(snk_user_room.count(self))snk_leave(self);
            auto rit=snk_rooms.find(code);
            if(rit==snk_rooms.end()){self->send_msg("SNK|noroom\n");return;}
            SnkRoom&r=rit->second;
            if(r.a==self){self->send_msg("SNK|err|你已在房间\n");return;}
            if(r.b){self->send_msg("SNK|full\n");return;}
            r.b=self;
            snk_user_room[self]=code;
            std::string rs=std::to_string(r.ra?1:0)+"|"+std::to_string(r.rb?1:0);
            if(r.a)r.a->send_msg("SNK|joined|"+code+"|a|"+rs+"\n");
            if(r.b)r.b->send_msg("SNK|joined|"+code+"|b|"+rs+"\n");
            return;
        }
        if(cmd=="ready"){
            auto it=snk_user_room.find(self);
            if(it==snk_user_room.end()){self->send_msg("SNK|err|不在房间\n");return;}
            auto rit=snk_rooms.find(it->second);
            if(rit==snk_rooms.end()){self->send_msg("SNK|left\n");return;}
            SnkRoom&r=rit->second;
            if(r.a==self)r.ra=true;
            else if(r.b==self)r.rb=true;
            else {self->send_msg("SNK|err|不在房间\n");return;}
            std::string rs=std::to_string(r.ra?1:0)+"|"+std::to_string(r.rb?1:0);
            if(r.a)r.a->send_msg("SNK|roomstate|"+rs+"\n");
            if(r.b)r.b->send_msg("SNK|roomstate|"+rs+"\n");
            // 双方都准备 → 开局（或一局结束后重开）
            if(r.ra&&r.rb&&(!r.g.running)){
                r.g.init(snk_now_ms(), r.food_count, r.speed_ms);
                r.ra=false;r.rb=false;
                std::string st="SNK|pinit|"+r.g.dumpA()+"|"+r.g.dumpB()+"|"
                    +r.g.dumpFoods()+"|0|0|";
                r.a->send_msg(st+"1\n");
                r.b->send_msg(st+"2\n");
            }
            return;
        }
        if(cmd=="leave"){
            snk_leave(self);self->send_msg("SNK|left\n");return;
        }
        if(cmd.rfind("pboost|",0)==0){
            int bv=atoi(cmd.c_str()+7);
            auto it=snk_user_room.find(self);
            if(it!=snk_user_room.end()){
                auto rit=snk_rooms.find(it->second);
                if(rit!=snk_rooms.end()){
                    SnkRoom&r=rit->second;
                    if(r.a==self)r.g.boostA=(bv==1);
                    else if(r.b==self)r.g.boostB=(bv==1);
                }
            }
            return;
        }
        if(cmd.rfind("pdir|",0)==0){
            const char* p=cmd.c_str()+5;
            int x=atoi(p),y=0;
            const char* q=strchr(p,'|');if(q)y=atoi(q+1);
            auto it=snk_user_room.find(self);
            if(it!=snk_user_room.end()){
                auto rit=snk_rooms.find(it->second);
                if(rit!=snk_rooms.end()){
                    SnkRoom&r=rit->second;
                    if(r.a==self)r.g.set_dirA(x,y);
                    else if(r.b==self)r.g.set_dirB(x,y);
                }
            }
            return;
        }
        self->send_msg("SNK|err|未知命令\n");return;}

        // ========== 坦克大战协议（TN|） ==========
    // ========== 枪战肉鸽协议（GUN|） ==========
    if(text.rfind("GUN|",0)==0){
        std::string cmd=text.substr(4);
        // ---- 联机房间：创建/加入/准备/离开 ----
        if(cmd.rfind("create",0)==0){
            if(gun_user_room.count(self)){self->send_msg("GUN|err|你已在房间里\n");return;}
            int d=1;
            if(cmd.size()>7&&cmd[6]=='|'){d=atoi(cmd.c_str()+7);}
            if(d<0)d=0; if(d>2)d=2;
            std::string code=gun_room_code();
            GunRoom r; r.code=code; r.a=self; r.diff=d; r.running=false;
            r.lastActive=snk_now_ms();
            gun_rooms[code]=std::move(r);
            gun_user_room[self]=code;
            self->send_msg("GUN|created|"+code+"\n");
            return;
        }
        if(cmd.rfind("join|",0)==0){
            std::string code=cmd.substr(5);
            if(gun_user_room.count(self))gun_leave(self);
            auto rit=gun_rooms.find(code);
            if(rit==gun_rooms.end()){self->send_msg("GUN|noroom\n");return;}
            GunRoom&r=rit->second;
            if(r.a==self||r.b==self){self->send_msg("GUN|err|你已在房间\n");return;}
            if(r.a&&r.b){self->send_msg("GUN|full\n");return;}
            if(!r.a)r.a=self; else r.b=self;   // 房主离开后新人可补位
            r.lastActive=snk_now_ms();
            gun_user_room[self]=code;
            std::string rs=std::to_string(r.ra?1:0)+"|"+std::to_string(r.rb?1:0);
            if(r.a)r.a->send_msg("GUN|joined|"+code+"|a|"+rs+"\n");
            if(r.b)r.b->send_msg("GUN|joined|"+code+"|b|"+rs+"\n");
            return;
        }
        if(cmd=="ready"){
            auto it=gun_user_room.find(self);
            if(it==gun_user_room.end()){self->send_msg("GUN|err|不在房间\n");return;}
            auto rit=gun_rooms.find(it->second);
            if(rit==gun_rooms.end()){self->send_msg("GUN|left\n");return;}
            GunRoom&r=rit->second;
            r.lastActive=snk_now_ms();
            if(r.a==self)r.ra=true;
            else if(r.b==self)r.rb=true;
            else {self->send_msg("GUN|err|不在房间\n");return;}
            std::string rs=std::to_string(r.ra?1:0)+"|"+std::to_string(r.rb?1:0);
            if(r.a)r.a->send_msg("GUN|roomstate|"+rs+"\n");
            if(r.b)r.b->send_msg("GUN|roomstate|"+rs+"\n");
            // 双方都准备 → 开局（或一局结束后重开）
            if(r.ra&&r.rb&&(!r.running||r.g.over)){
                r.g.init(r.diff, 2, snk_now_ms());
                r.running=true;
                r.ra=false;r.rb=false;
                std::string base="GUN|pinit|"+r.code+"|"+std::to_string(r.diff)+"|"
                    +std::to_string(r.g.wave.totalWaves)+"|"+std::to_string(r.g.wave.maxLeak);
                if(r.a)r.a->send_msg(base+"|a\n");
                if(r.b)r.b->send_msg(base+"|b\n");
            }
            return;
        }
        if(cmd=="leave"){
            gun_leave(self);self->send_msg("GUN|left\n");return;
        }
        if(cmd.rfind("new",0)==0){
            gun::Engine&g=gun_games[self];
            int d=1;  // 默认中等
            sscanf(cmd.c_str()+3,"|%d",&d);
            if(d<0)d=0; if(d>2)d=2;
            g.init(d, 1, snk_now_ms());
            self->send_msg("GUN|init|"+std::to_string(g.diff)+"|"
                +std::to_string(g.wave.totalWaves)+"|"+std::to_string(g.wave.maxLeak)+"\n");
            return;
        }
        if(cmd=="quit"){
            gun_games.erase(self);
            return;
        }
        if(cmd.rfind("move|",0)==0){
            const char* p=cmd.c_str()+5;
            int x=atoi(p),y=0;
            const char* q=strchr(p,'|');if(q)y=atoi(q+1);
            int pi=gun_pi(self);
            if(pi>=0){
                auto it=gun_user_room.find(self);
                auto rit=gun_rooms.find(it->second);
                if(rit!=gun_rooms.end()&&rit->second.running)rit->second.g.set_move(pi,x,y);
            }else{
                auto it=gun_games.find(self);
                if(it!=gun_games.end())it->second.set_move(0,x,y);
            }
            return;
        }
        if(cmd.rfind("angle|",0)==0){
            double a=atof(cmd.c_str()+6);
            int pi=gun_pi(self);
            if(pi>=0){
                auto it=gun_user_room.find(self);
                auto rit=gun_rooms.find(it->second);
                if(rit!=gun_rooms.end()&&rit->second.running)rit->second.g.set_angle(pi,a);
            }else{
                auto it=gun_games.find(self);
                if(it!=gun_games.end())it->second.set_angle(0,a);
            }
            return;
        }
        if(cmd.rfind("choice|",0)==0){
            int n=atoi(cmd.c_str()+7);
            int pi=gun_pi(self);
            if(pi>=0){
                auto it=gun_user_room.find(self);
                auto rit=gun_rooms.find(it->second);
                if(rit!=gun_rooms.end()&&rit->second.running)rit->second.g.apply_choice(pi,n);
            }else{
                auto it=gun_games.find(self);
                if(it!=gun_games.end())it->second.apply_choice(0,n);
            }
            return;
        }
        if(cmd.rfind("mode|",0)==0){
            int m=atoi(cmd.c_str()+5);
            int pi=gun_pi(self);
            if(pi>=0){
                auto it=gun_user_room.find(self);
                auto rit=gun_rooms.find(it->second);
                if(rit!=gun_rooms.end()&&rit->second.running)rit->second.g.set_fire_mode(pi,m);
            }else{
                auto it=gun_games.find(self);
                if(it!=gun_games.end())it->second.set_fire_mode(0,m);
            }
            return;
        }
        if(cmd.rfind("pause|",0)==0){
            int p=atoi(cmd.c_str()+6);
            auto it=gun_user_room.find(self);
            if(it!=gun_user_room.end()){
                auto rit=gun_rooms.find(it->second);
                if(rit!=gun_rooms.end()&&rit->second.running){rit->second.g.set_paused(p==1);return;}
            }
            auto gi=gun_games.find(self);
            if(gi!=gun_games.end())gi->second.set_paused(p==1);
            return;
        }
        if(cmd=="fire"){
            int pi=gun_pi(self);
            if(pi>=0){
                auto it=gun_user_room.find(self);
                auto rit=gun_rooms.find(it->second);
                if(rit!=gun_rooms.end()&&rit->second.running)rit->second.g.fire_manual(pi);
            }else{
                auto it=gun_games.find(self);
                if(it!=gun_games.end())it->second.fire_manual(0);
            }
            return;
        }
        if(cmd=="fireoff"){
            int pi=gun_pi(self);
            if(pi>=0){
                auto it=gun_user_room.find(self);
                auto rit=gun_rooms.find(it->second);
                if(rit!=gun_rooms.end()&&rit->second.running)rit->second.g.fire_manual_off(pi);
            }else{
                auto it=gun_games.find(self);
                if(it!=gun_games.end())it->second.fire_manual_off(0);
            }
            return;
        }
        self->send_msg("GUN|err|未知命令\n");return;
    }
    if(text.rfind("TN|",0)==0){
        std::string cmd=text.substr(3);
        if(cmd.rfind("new",0)==0){
            tn::Engine&g=tn_games[self];
            g.init(snk_now_ms());
            self->send_msg("TN|init|"+g.dump_map()+"|0|3\n");
            return;
        }
        if(cmd=="quit"){   // 退出/停止单机对局
            auto it=tn_games.find(self);
            if(it!=tn_games.end()) tn_games.erase(it);
            return;
        }
        if(cmd.rfind("move|",0)==0){
            const char* p=cmd.c_str()+5;
            int x=atoi(p),y=0;
            const char* q=strchr(p,'|');if(q)y=atoi(q+1);
            auto it=tn_games.find(self);
            if(it!=tn_games.end())it->second.set_move(x,y);
            return;
        }
        if(cmd=="stop"){
            auto it=tn_games.find(self);
            if(it!=tn_games.end())it->second.stop();
            return;
        }
        if(cmd=="shoot"){
            auto it=tn_games.find(self);
            if(it!=tn_games.end())it->second.shoot();
            return;
        }
        // ---- 双人 ----
        if(cmd.rfind("create",0)==0){
            if(tn_user_room.count(self)){self->send_msg("TN|err|你已在房间里\n");return;}
            int gsize=24, lives=3;
            sscanf(cmd.c_str()+6,"|%d|%d",&gsize,&lives);
            if(gsize<16)gsize=16; if(gsize>32)gsize=32;
            if(lives<1)lives=1; if(lives>9)lives=9;
            std::string code=tn_room_code();
            TnRoom r;r.code=code;r.a=self;r.ra=false;r.rb=false;r.gsize=gsize;r.lives=lives;
            tn_rooms[code]=std::move(r);
            tn_user_room[self]=code;
            self->send_msg("TN|created|"+code+"\n");
            return;
        }
        if(cmd.rfind("join|",0)==0){
            std::string code=cmd.substr(5);
            if(tn_user_room.count(self))tn_leave(self);
            auto rit=tn_rooms.find(code);
            if(rit==tn_rooms.end()){self->send_msg("TN|noroom\n");return;}
            TnRoom&r=rit->second;
            if(r.a==self){self->send_msg("TN|err|你已在房间\n");return;}
            if(r.b){self->send_msg("TN|full\n");return;}
            r.b=self;
            tn_user_room[self]=code;
            std::string rs=std::to_string(r.ra?1:0)+"|"+std::to_string(r.rb?1:0);
            if(r.a)r.a->send_msg("TN|joined|"+code+"|a|"+rs+"\n");
            if(r.b)r.b->send_msg("TN|joined|"+code+"|b|"+rs+"\n");
            return;
        }
        if(cmd=="ready"){
            auto it=tn_user_room.find(self);
            if(it==tn_user_room.end()){self->send_msg("TN|err|不在房间\n");return;}
            auto rit=tn_rooms.find(it->second);
            if(rit==tn_rooms.end()){self->send_msg("TN|left\n");return;}
            TnRoom&r=rit->second;
            if(r.a==self)r.ra=true;
            else if(r.b==self)r.rb=true;
            else {self->send_msg("TN|err|不在房间\n");return;}
            std::string rs=std::to_string(r.ra?1:0)+"|"+std::to_string(r.rb?1:0);
            if(r.a)r.a->send_msg("TN|roomstate|"+rs+"\n");
            if(r.b)r.b->send_msg("TN|roomstate|"+rs+"\n");
            if(r.ra&&r.rb&&(!r.g.running)){
                r.g.init(snk_now_ms(), r.gsize, r.lives);
                r.ra=false;r.rb=false;
                char tnbuf[64];
                snprintf(tnbuf,sizeof(tnbuf),"%.1f,%.1f,%d,%d,%d,%d,%d",r.g.ta.x,r.g.ta.y,r.g.ta.dir,r.g.ta.hp,r.g.ta.alive?1:0,r.g.ta.ammo,r.g.ta.reload);
                std::string st="TN|pinit|"+r.g.dump_map()+"|"+std::string(tnbuf)+"|";
                snprintf(tnbuf,sizeof(tnbuf),"%.1f,%.1f,%d,%d,%d,%d,%d",r.g.tb.x,r.g.tb.y,r.g.tb.dir,r.g.tb.hp,r.g.tb.alive?1:0,r.g.tb.ammo,r.g.tb.reload);
                st+=std::string(tnbuf)+"|";
                if(r.a)r.a->send_msg(st+"1|"+std::to_string(r.g.livesA)+"|"+std::to_string(r.g.livesB)+"\n");
                if(r.b)r.b->send_msg(st+"2|"+std::to_string(r.g.livesA)+"|"+std::to_string(r.g.livesB)+"\n");
            }
            return;
        }
        if(cmd=="leave"){
            tn_leave(self);self->send_msg("TN|left\n");return;
        }
        if(cmd.rfind("rejoin|",0)==0){   // 断线重连：补回原角色并同步对局状态
            std::string rest=cmd.substr(7);
            size_t pos=rest.find('|');
            std::string code=pos==std::string::npos?"":rest.substr(0,pos);
            int role=pos==std::string::npos?0:atoi(rest.c_str()+pos+1);
            if(tn_user_room.count(self))tn_leave(self);
            auto rit=tn_rooms.find(code);
            if(rit==tn_rooms.end()){self->send_msg("TN|noroom\n");return;}
            TnRoom&r=rit->second;
            if(role==1&&r.a==nullptr)r.a=self;
            else if(role==2&&r.b==nullptr)r.b=self;
            else {self->send_msg("TN|err|房间不可重入\n");return;}
            tn_user_room[self]=code;
            std::string rs=std::to_string(r.ra?1:0)+"|"+std::to_string(r.rb?1:0);
            if(r.a)r.a->send_msg("TN|joined|"+code+"|a|"+rs+"\n");
            if(r.b)r.b->send_msg("TN|joined|"+code+"|b|"+rs+"\n");
            if(r.g.running){   // 对局进行中：给双方补发完整状态
                char tnbuf[64];
                snprintf(tnbuf,sizeof(tnbuf),"%.1f,%.1f,%d,%d,%d,%d,%d",r.g.ta.x,r.g.ta.y,r.g.ta.dir,r.g.ta.hp,r.g.ta.alive?1:0,r.g.ta.ammo,r.g.ta.reload);
                std::string st="TN|pinit|"+r.g.dump_map()+"|"+std::string(tnbuf)+"|";
                snprintf(tnbuf,sizeof(tnbuf),"%.1f,%.1f,%d,%d,%d,%d,%d",r.g.tb.x,r.g.tb.y,r.g.tb.dir,r.g.tb.hp,r.g.tb.alive?1:0,r.g.tb.ammo,r.g.tb.reload);
                st+=std::string(tnbuf)+"|";
                if(r.a)r.a->send_msg(st+"1|"+std::to_string(r.g.livesA)+"|"+std::to_string(r.g.livesB)+"\n");
                if(r.b)r.b->send_msg(st+"2|"+std::to_string(r.g.livesA)+"|"+std::to_string(r.g.livesB)+"\n");
            }
            return;
        }
        if(cmd.rfind("pmove|",0)==0){
            const char* p=cmd.c_str()+6;
            int x=atoi(p),y=0;
            const char* q=strchr(p,'|');if(q)y=atoi(q+1);
            auto it=tn_user_room.find(self);
            if(it!=tn_user_room.end()){
                auto rit=tn_rooms.find(it->second);
                if(rit!=tn_rooms.end()){
                    TnRoom&r=rit->second;
                    if(r.a==self)r.g.set_moveA(x,y);
                    else if(r.b==self)r.g.set_moveB(x,y);
                }
            }
            return;
        }
        if(cmd=="pstop"){
            auto it=tn_user_room.find(self);
            if(it!=tn_user_room.end()){
                auto rit=tn_rooms.find(it->second);
                if(rit!=tn_rooms.end()){
                    TnRoom&r=rit->second;
                    if(r.a==self)r.g.stopA();
                    else if(r.b==self)r.g.stopB();
                }
            }
            return;
        }
        if(cmd=="pshoot"){
            auto it=tn_user_room.find(self);
            if(it!=tn_user_room.end()){
                auto rit=tn_rooms.find(it->second);
                if(rit!=tn_rooms.end()){
                    TnRoom&r=rit->second;
                    if(r.a==self)r.g.shootA();
                    else if(r.b==self)r.g.shootB();
                }
            }
            return;
        }
        self->send_msg("TN|err|未知命令\n");return;}

        // ========== 三消协议（T3|） v2（平面/散点/服务器权威 blocked） ==========
    if(text.rfind("T3|",0)==0){
        std::string cmd=text.substr(3);
        static std::mt19937 t3rng((unsigned)time(nullptr));
        if(cmd.rfind("new",0)==0){
            int W=4,H=3,L=3,NP=4;
            sscanf(cmd.c_str()+3,"|%d|%d|%d|%d",&W,&H,&L,&NP);
            if(W<2||W>6||H<2||H>5||L<1||L>4||NP<2||NP>8||(W*H*L)%NP!=0){
                self->send_msg("T3|err|参数不合法\n");return;}
            tmg::GameState gs;
            if(!tmg::Engine::generate(W,H,L,NP,t3rng,gs)){
                self->send_msg("T3|err|生成失败，请重试\n");return;}
            tmg::Engine::refresh_blocked(gs);
            t3_games[self]=gs;
            std::string resp="T3|init|"+std::to_string(W)+"|"+std::to_string(H)+"|"+std::to_string(L)+"|"+std::to_string(NP)+"|";
            resp+=tmg::Engine::dump(gs);
            resp+="\n";self->send_msg(resp);return;}
        if(cmd.rfind("pick|",0)==0){
            int id=atoi(cmd.c_str()+5);
            auto it=t3_games.find(self);
            if(it==t3_games.end()){self->send_msg("T3|err|请先开新局\n");return;}
            tmg::GameState& gs=it->second;
            if(gs.over){self->send_msg("T3|err|本局已结束，请开新局\n");return;}
            std::vector<int> cl;
            int rc=tmg::Engine::pick(gs,id,cl);
            if(rc==1){self->send_msg("T3|err|该牌不可点\n");return;}
            if(rc==3){
                tmg::Engine::refresh_blocked(gs);
                std::string r="T3|clear|";
                for(int cc:cl){r+=std::to_string(cc)+",";}
                r+="\n";self->send_msg(r);
                self->send_msg("T3|reblock|"+tmg::Engine::dump_reblock(gs)+"\n");
                if(gs.win)self->send_msg("T3|win\n");
                else if(tmg::Engine::is_stuck(gs))self->send_msg("T3|stuck\n");
                return;}
            if(rc==2){self->send_msg("T3|fail\n");return;}
            tmg::Engine::refresh_blocked(gs);
            self->send_msg("T3|pickok|"+std::to_string(id)+"|"+std::to_string((int)gs.slot.size())+"\n");
            self->send_msg("T3|reblock|"+tmg::Engine::dump_reblock(gs)+"\n");
            if(tmg::Engine::is_stuck(gs))self->send_msg("T3|stuck\n");
            return;}
        if(cmd=="shuffle"){
            auto it=t3_games.find(self);
            if(it==t3_games.end()){self->send_msg("T3|err|请先开新局\n");return;}
            if(!tmg::Engine::shuffle(it->second,t3rng)){
                self->send_msg("T3|err|剩余牌无法重排成可解局面，请重新开局\n");return;}
            self->send_msg("T3|shuffled|"+tmg::Engine::dump(it->second)+"\n");
            return;}
        if(cmd=="seq"){
            auto it=t3_games.find(self);
            if(it==t3_games.end()){self->send_msg("T3|err|请先开新局\n");return;}
            std::vector<std::vector<int>> out;
            if(tmg::Engine::solve_sequence(it->second,out)){
                std::string r="T3|seq|";
                for(auto&grp:out){
                    for(int i=0;i<3;i++){r+=std::to_string(grp[i]);if(i<2)r+=",";}
                    r+=";";
                }
                r+="\n";self->send_msg(r);
            }else self->send_msg("T3|err|当前局面无解\n");
            return;}
        self->send_msg("T3|err|未知命令\n");return;}

// ========== 五子棋协议（G5|） ==========
    if(text.rfind("G5|",0)==0){
        std::string cmd=text.substr(3);
        if(cmd.rfind("create",0)==0||cmd=="leave"||cmd.rfind("join|",0)==0){
            std::lock_guard<std::mutex> lk(g5_mtx);
            if(cmd.rfind("create",0)==0){
                if(g5_user_room.count(self)){self->send_msg("G5|already\n");return;}
                std::string code=gen_room_code();
                G5Room r;r.code=code;r.b=self;r.b_token=self->token;
                g5_rooms[code]=std::move(r);g5_user_room[self]=code;
                self->send_msg("G5|created|"+code+"\n");return;
            }
            if(cmd=="leave"){
                g5_leave(self);self->send_msg("G5|left\n");return;
            }
            // join|CODE —— 若在别的五子棋房间自动离开
            std::string code=cmd.substr(5);
            if(g5_user_room.count(self))g5_leave(self);
            auto rit=g5_rooms.find(code);
            if(rit==g5_rooms.end()){self->send_msg("G5|noroom\n");return;}
            G5Room&r=rit->second;
            bool b_occ=(r.b!=nullptr)||!r.b_token.empty();
            bool w_occ=(r.w!=nullptr)||!r.w_token.empty();
            if(b_occ&&w_occ){self->send_msg("G5|full\n");return;}
            if(!b_occ){r.b=self;r.b_token=self->token;}
            else {r.w=self;r.w_token=self->token;}
            g5_user_room[self]=code;
            std::string rs=std::to_string(r.b_ready?1:0)+"|"+std::to_string(r.w_ready?1:0);
            if(r.b)r.b->send_msg("G5|joined|"+code+"|b|"+rs+"\n");
            if(r.w)r.w->send_msg("G5|joined|"+code+"|w|"+rs+"\n");
            return;
        }
        bool in5=false;{std::lock_guard<std::mutex> lk(g5_mtx);in5=g5_user_room.count(self)>0;}
        if(in5){
            std::lock_guard<std::mutex> lk(g5_mtx);
            auto it=g5_user_room.find(self);
            if(it==g5_user_room.end()){self->send_msg("G5|left\n");return;}
            auto rit=g5_rooms.find(it->second);
            if(rit==g5_rooms.end()){g5_user_room.erase(it);self->send_msg("G5|left\n");return;}
            G5Room&r=rit->second;
            int my=(r.b==self)?1:2;
            if(cmd=="ready"){
                if(my==1)r.b_ready=true;else r.w_ready=true;
                std::string rs=std::to_string(r.b_ready?1:0)+"|"+std::to_string(r.w_ready?1:0);
                g5_broadcast(r,"G5|roomstate|"+rs+"\n");
                // 首次开始，或一局结束后双方再准备 → 重置开新一局
                if(r.b_ready&&r.w_ready&&(!r.started||r.over)){
                    memset(r.g,0,sizeof(r.g));
                    r.hist.clear();r.mv_side.clear();
                    r.over=false;r.winner=0;r.turn=1;
                    r.b_ready=false;r.w_ready=false;   // 新一局的准备状态复位
                    r.started=true;
                    if(r.b)r.b->send_msg("G5|start|"+r.code+"|b\n");
                    if(r.w)r.w->send_msg("G5|start|"+r.code+"|w\n");
                }
                return;
            }
            if(cmd.rfind("move|",0)==0){
                int x=-1,y=-1;
                if(sscanf(cmd.c_str()+5,"%d,%d",&x,&y)!=2){self->send_msg("G5|bad\n");return;}
                if(!r.started||r.over){self->send_msg("G5|over\n");return;}
                if(x<0||x>=15||y<0||y>=15||r.g[y][x]!=0){self->send_msg("G5|bad\n");return;}
                if(r.turn!=my){self->send_msg("G5|notturn\n");return;}
                r.g[y][x]=my;r.hist.push_back({x,y});r.mv_side.push_back(my);
                std::string resp="G5|mv|"+std::to_string(x)+","+std::to_string(y)+"|"+(my==1?"b":"w");
                if(g5_check(r.g,x,y,my)){
                    r.over=true;r.winner=my;
                    resp+="|over|"+std::to_string(my);
                }else{
                    r.turn=(r.turn==1)?2:1;
                }
                resp+="\n";
                g5_broadcast(r,resp);
                return;
            }
            if(cmd=="undo_req"){
                if(!r.started||r.over){self->send_msg("G5|noundo\n");return;}
                if(r.undo_pending){self->send_msg("G5|undo_pending\n");return;}
                if(r.hist.empty()){self->send_msg("G5|noundo\n");return;}
                bool last_is_mine=!r.mv_side.empty()&&r.mv_side.back()==my;
                bool can=last_is_mine;
                if(!can&&r.mv_side.size()>=2&&r.mv_side[r.mv_side.size()-2]==my)can=true;
                if(!can){self->send_msg("G5|noundo\n");return;}
                r.undo_pending=true;r.undo_who=my;
                self->send_msg("G5|undo_wait\n");
                ChatUser*other=(r.b==self)?r.w:r.b;
                if(other)other->send_msg(std::string("G5|undo_req_from|")+(my==1?"b":"w")+"\n");
                return;
            }
            if(cmd.rfind("undo_ans|",0)==0){
                if(!r.undo_pending){self->send_msg("G5|noundo\n");return;}
                if(my==r.undo_who){self->send_msg("G5|noperm\n");return;}
                bool yes=(cmd.size()>=11&&cmd.compare(9,3,"yes")==0);
                r.undo_pending=false;
                if(yes){
                    int back=1;
                    if(r.mv_side.empty())back=0;
                    else if(r.mv_side.back()!=r.undo_who)back=2;
                    if(back>(int)r.hist.size())back=(int)r.hist.size();
                    for(int i=0;i<back;i++){
                        auto p=r.hist.back();
                        if(p.first>=0&&p.first<15&&p.second>=0&&p.second<15)r.g[p.second][p.first]=0;
                        r.hist.pop_back();r.mv_side.pop_back();
                    }
                    r.turn=r.undo_who;   // 回到悔棋者落子前
                    g5_broadcast(r,"G5|undo|ok|"+std::to_string(r.turn)+"\n");
                }else{
                    ChatUser*who=(r.undo_who==1)?r.b:r.w;
                    if(who)who->send_msg("G5|undo_no\n");
                }
                return;
            }
            if(cmd=="state"){
                std::string resp="G5|state|";
                for(int y=0;y<15;y++)for(int x=0;x<15;x++)if(r.g[y][x]){
                    resp+=std::to_string(x)+","+std::to_string(y)+","+std::to_string(r.g[y][x])+";";
                }
                resp+=std::string("|turn|")+(r.turn==1?"b":"w");
                resp+=std::string("|over|")+(r.over?std::to_string(r.winner):"0");
                resp+="\n";
                self->send_msg(resp);return;
            }
            self->send_msg("G5|badcmd\n");return;
        }
        self->send_msg("G5|badcmd\n");return;
    }

    // ========== 中国象棋协议（服务器权威校验） ==========
    if(text.rfind("GAME|",0)==0){
        std::string cmd=text.substr(5);
        // ---- 房间管理命令：create / join|CODE / leave ----
        if(cmd.rfind("create",0)==0||cmd=="leave"||cmd.rfind("join|",0)==0){
            std::lock_guard<std::mutex> lk(g_room_mtx);
            if(cmd.rfind("create",0)==0){
                if(g_user_room.count(self)){self->send_msg("GAME|already\n");return;}
                int total=0,step=0;char side='r';
                // GAME|create|total|step|side
                size_t p1=cmd.find('|');
                if(p1!=std::string::npos){
                    size_t p2=cmd.find('|',p1+1);
                    total=atoi(cmd.c_str()+p1+1);
                    if(p2!=std::string::npos){
                        size_t p3=cmd.find('|',p2+1);
                        step=atoi(cmd.c_str()+p2+1);
                        if(p3!=std::string::npos)side=cmd[p3+1];
                    }
                }
                if(total<0)total=0;if(total>7200)total=7200;
                if(step<0)step=0;if(step>600)step=600;
                if(side!='b')side='r';
                std::string code=gen_room_code();
                GameRoom r;r.code=code;r.eng.reset();
                r.total_time=total;r.step_time=step;
                if(side=='b'){r.black=self;r.black_token=self->token;}   // 创建者可选执先手/后手
                else {r.red=self;r.red_token=self->token;}
                g_rooms[code]=std::move(r);g_user_room[self]=code;
                self->send_msg("GAME|created|"+code+"\n");return;
            }
            if(cmd=="leave"){
                leave_room(self);self->send_msg("GAME|left\n");return;
            }
            // join|CODE —— 若已在其他房间，自动离开旧房间再加入（点新邀请应进新房间）
            std::string code=cmd.substr(5);
            if(g_user_room.count(self)){
                leave_room(self);   // 离开旧房间（会通知旧房间对方）
            }
            auto rit=g_rooms.find(code);
            if(rit==g_rooms.end()){self->send_msg("GAME|noroom\n");return;}
            GameRoom&r=rit->second;
            // 座位占用：在线指针 或 离线待恢复的 token 都算占用
            bool red_occ=(r.red!=nullptr)||!r.red_token.empty();
            bool black_occ=(r.black!=nullptr)||!r.black_token.empty();
            if(red_occ&&black_occ){self->send_msg("GAME|full\n");return;}
            bool iam_red=false;
            if(!red_occ){r.red=self;r.red_token=self->token;iam_red=true;}        // 红方空则补红
            else {r.black=self;r.black_token=self->token;}                        // 否则补黑
            g_user_room[self]=code;
            // 双方就位 → 进入等待准备阶段（不立即开始）
            std::string tp=std::to_string(r.total_time)+"|"+std::to_string(r.step_time);
            std::string rs=std::to_string(r.red_ready?1:0)+"|"+std::to_string(r.black_ready?1:0);
            if(r.red)r.red->send_msg("GAME|joined|"+code+"|r|"+tp+"|"+rs+"\n");
            if(r.black)r.black->send_msg("GAME|joined|"+code+"|b|"+tp+"|"+rs+"\n");
            (void)iam_red;
            return;
        }
        // ---- 判断是否在房间 ----
        bool inroom=false;
        {std::lock_guard<std::mutex> lk(g_room_mtx);inroom=g_user_room.count(self)>0;}
        if(inroom){
            std::lock_guard<std::mutex> lk(g_room_mtx);
            auto it=g_user_room.find(self);
            if(it==g_user_room.end()){self->send_msg("GAME|left\n");return;}
            auto rit=g_rooms.find(it->second);
            if(rit==g_rooms.end()){g_user_room.erase(it);self->send_msg("GAME|left\n");return;}
            GameRoom&r=rit->second;
            xq::Side my=(r.red==self)?xq::RED:xq::BLACK;
            if(cmd.rfind("move|",0)==0){
                int fx=0,fy=0,tx=0,ty=0;
                if(sscanf(cmd.c_str()+5,"%d,%d,%d,%d",&fx,&fy,&tx,&ty)!=4){self->send_msg("GAME|illegal\n");return;}
                if(r.eng.game_over){room_broadcast(r,"GAME|over\n");return;}
                if(r.eng.turn!=my){self->send_msg("GAME|notturn\n");return;}
                if(fx<0||fx>8||fy<0||fy>9||tx<0||tx>8||ty<0||ty>9){self->send_msg("GAME|illegal\n");return;}
                if(r.eng.b[fy][fx].type==xq::NONE||r.eng.b[fy][fx].side!=r.eng.turn){self->send_msg("GAME|illegal\n");return;}
                if(!r.eng.isLegalMove(fx,fy,tx,ty)){self->send_msg("GAME|illegal\n");return;}
                r.hist.push_back(r.eng);   // 保存历史局面（联机悔棋用）
                r.movers.push_back(my);     // 记录走子方
                r.eng.makeMove(fx,fy,tx,ty);
                // ---- 计时：扣除走子方本步用时 ----
                time_t now=time(nullptr);
                if(r.total_time>0||r.step_time>0){
                    if(my==xq::RED){int used=(int)(now-r.red_step_start);if(r.step_time>0&&used>r.step_time)used=r.step_time;if(r.total_time>0){r.red_left-=used;if(r.red_left<0)r.red_left=0;}r.black_step_start=now;}
                    else         {int used=(int)(now-r.black_step_start);if(r.step_time>0&&used>r.step_time)used=r.step_time;if(r.total_time>0){r.black_left-=used;if(r.black_left<0)r.black_left=0;}r.red_step_start=now;}
                }
                std::string resp="GAME|ok|"+std::to_string(fx)+","+std::to_string(fy)+","+std::to_string(tx)+","+std::to_string(ty);
                if(r.eng.inCheck(r.eng.turn))resp+="|check";
                if(r.eng.game_over)resp+=std::string("|over|")+(r.eng.turn==xq::RED?"BLACK":"RED");
                resp+="\n";
                room_broadcast(r,resp);
                // 广播剩余时间
                room_broadcast(r,"GAME|time|"+std::to_string(r.red_left)+"|"+std::to_string(r.black_left)+"\n");
                return;
            }
            // ---- 联机悔棋：请求 → 对方同意/拒绝 ----
            if(cmd=="undo_req"){
                if(!r.started||r.eng.game_over){self->send_msg("GAME|noundo\n");return;}
                if(r.undo_pending){self->send_msg("GAME|undo_pending\n");return;}
                if(r.hist.empty()){self->send_msg("GAME|noundo\n");return;}
                // 悔棋者必须确实走过一步：要么最后一步是我走的（对方没落子，撤1步），
                // 要么倒数第二步是我走的（轮到我了，撤2步）
                bool last_is_mine=!r.movers.empty()&&r.movers.back()==my;
                bool can=false;
                if(last_is_mine)can=true;
                else if(r.movers.size()>=2&&r.movers[r.movers.size()-2]==my)can=true;
                if(!can){self->send_msg("GAME|noundo\n");return;}
                r.undo_pending=true;r.undo_who=my;
                self->send_msg("GAME|undo_wait\n");
                ChatUser*other=(r.red==self)?r.black:r.red;
                if(other)other->send_msg(std::string("GAME|undo_req_from|")+(my==xq::RED?"r":"b")+"\n");
                return;
            }
            if(cmd.rfind("undo_ans|",0)==0){
                if(!r.undo_pending){self->send_msg("GAME|noundo\n");return;}
                if(my==r.undo_who){self->send_msg("GAME|noperm\n");return;}   // 请求方不能应答
                bool yes=(cmd.size()>=12&&cmd.compare(9,3,"yes")==0);
                r.undo_pending=false;
                if(yes){
                    // 回退步数：最后一步是我走（对方没走）→撤1步；轮到我了→撤2步
                    int back=1;
                    if(r.movers.empty())back=0;
                    else if(r.movers.back()!=r.undo_who)back=2;   // 最后一步不是悔棋者→悔棋者上一步在倒数第2
                    if(back>(int)r.hist.size())back=(int)r.hist.size();
                    for(int i=0;i<back;i++){r.eng=r.hist.back();r.hist.pop_back();r.movers.pop_back();}
                    time_t nw=time(nullptr);
                    r.red_step_start=nw;r.black_step_start=nw;   // 重置步时，防止误判超时
                    room_broadcast(r,std::string("GAME|undo|ok|")+(r.eng.turn==xq::RED?"r":"b")+"\n");
                }else{
                    if(r.undo_who==xq::RED&&r.red)r.red->send_msg("GAME|undo_no\n");
                    else if(r.undo_who==xq::BLACK&&r.black)r.black->send_msg("GAME|undo_no\n");
                }
                return;
            }
            if(cmd=="ready"){
                if(r.red==self)r.red_ready=true;
                else if(r.black==self)r.black_ready=true;
                else {self->send_msg("GAME|noperm\n");return;}
                std::string rs=std::to_string(r.red_ready?1:0)+"|"+std::to_string(r.black_ready?1:0);
                room_broadcast(r,"GAME|roomstate|"+rs+"\n");
                // 双方都准备 → 开始对局
                if(r.red_ready&&r.black_ready&&!r.started){
                    r.started=true;r.eng.reset();
                    time_t now=time(nullptr);
                    if(r.total_time>0){r.red_left=r.total_time;r.black_left=r.total_time;}
                    r.red_step_start=now;r.black_step_start=now;
                    std::string tp=std::to_string(r.total_time)+"|"+std::to_string(r.step_time);
                    if(r.red)r.red->send_msg("GAME|start|"+r.code+"|r|"+tp+"\n");
                    if(r.black)r.black->send_msg("GAME|start|"+r.code+"|b|"+tp+"\n");
                }
                return;
            }
            if(cmd=="turn"){self->send_msg(std::string("GAME|turn|")+(r.eng.turn==xq::RED?"r":"b")+"\n");return;}
            if(cmd=="state"){
                std::string resp="GAME|state|";
                for(int y=0;y<10;y++)for(int x=0;x<9;x++){
                    auto p=r.eng.b[y][x];
                    if(p.type==xq::NONE)continue;
                    char ch='?';
                    switch(p.type){case xq::KING:ch='k';break;case xq::ADVISOR:ch='a';break;case xq::ELEPHANT:ch='e';break;case xq::HORSE:ch='h';break;case xq::ROOK:ch='r';break;case xq::CANNON:ch='c';break;case xq::PAWN:ch='p';break;default:break;}
                    resp+=ch;resp+=(p.side==xq::RED?'r':'b');
                    resp+=std::to_string(x)+std::to_string(y)+";";
                }
                resp+=std::string("|turn|")+(r.eng.turn==xq::RED?"r":"b");
                resp+="|"+std::to_string(r.red_left)+"|"+std::to_string(r.black_left)+"\n";
                self->send_msg(resp);return;}
            if(cmd.rfind("legal|",0)==0){
                int x=0,y=0;
                if(sscanf(cmd.c_str()+6,"%d,%d",&x,&y)!=2){self->send_msg("GAME|legal|\n");return;}
                auto mv=r.eng.genLegal(x,y);
                std::string resp="GAME|legal|";
                for(auto&m:mv){resp+=std::to_string(m.tx)+","+std::to_string(m.ty)+";";}
                resp+="\n";self->send_msg(resp);return;
            }
            self->send_msg("GAME|badcmd\n");return;
        }
        // ---- 单机模式（无房间，全局引擎，兼容第三步） ----
        std::lock_guard<std::mutex> lk(g_chess_mtx);
        if(cmd=="new"){g_chess.reset();g_chess_hist.clear();self->send_msg("GAME|new|ok\n");return;}
        if(cmd=="turn"){self->send_msg(std::string("GAME|turn|")+(g_chess.turn==xq::RED?"r":"b")+"\n");return;}
        if(cmd=="undo"){
            if(g_chess_hist.empty()){self->send_msg("GAME|noundo\n");return;}
            g_chess=g_chess_hist.back();g_chess_hist.pop_back();
            self->send_msg(std::string("GAME|undo|ok|")+(g_chess.turn==xq::RED?"r":"b")+"\n");return;}
        if(cmd.rfind("move|",0)==0){
            int fx=0,fy=0,tx=0,ty=0;
            if(sscanf(cmd.c_str()+5,"%d,%d,%d,%d",&fx,&fy,&tx,&ty)!=4){self->send_msg("GAME|illegal\n");return;}
            if(g_chess.game_over){self->send_msg("GAME|over\n");return;}
            if(fx<0||fx>8||fy<0||fy>9||tx<0||tx>8||ty<0||ty>9){self->send_msg("GAME|illegal\n");return;}
            if(g_chess.b[fy][fx].type==xq::NONE||g_chess.b[fy][fx].side!=g_chess.turn){self->send_msg("GAME|illegal\n");return;}
            if(!g_chess.isLegalMove(fx,fy,tx,ty)){self->send_msg("GAME|illegal\n");return;}
            g_chess_hist.push_back(g_chess);
            g_chess.makeMove(fx,fy,tx,ty);
            std::string resp="GAME|ok|"+std::to_string(fx)+","+std::to_string(fy)+","+std::to_string(tx)+","+std::to_string(ty);
            if(g_chess.inCheck(g_chess.turn))resp+="|check";
            if(g_chess.game_over)resp+=std::string("|over|")+(g_chess.turn==xq::RED?"BLACK":"RED");
            resp+="\n";self->send_msg(resp);return;}
        if(cmd.rfind("legal|",0)==0){
            int x=0,y=0;
            if(sscanf(cmd.c_str()+6,"%d,%d",&x,&y)!=2){self->send_msg("GAME|legal|\n");return;}
            auto mv=g_chess.genLegal(x,y);
            std::string resp="GAME|legal|";
            for(auto&m:mv){resp+=std::to_string(m.tx)+","+std::to_string(m.ty)+";";}
            resp+="\n";self->send_msg(resp);return;}
        if(cmd=="state"){
            std::string resp="GAME|state|";
            for(int y=0;y<10;y++)for(int x=0;x<9;x++){
                auto p=g_chess.b[y][x];
                if(p.type==xq::NONE)continue;
                char ch='?';
                switch(p.type){case xq::KING:ch='k';break;case xq::ADVISOR:ch='a';break;case xq::ELEPHANT:ch='e';break;case xq::HORSE:ch='h';break;case xq::ROOK:ch='r';break;case xq::CANNON:ch='c';break;case xq::PAWN:ch='p';break;default:break;}
                resp+=ch;resp+=(p.side==xq::RED?'r':'b');
                resp+=std::to_string(x)+std::to_string(y)+";";
            }
            resp+=std::string("|turn|")+(g_chess.turn==xq::RED?"r":"b")+"\n";
            self->send_msg(resp);return;}
        self->send_msg("GAME|badcmd\n");return;}
    const char*ln=text.c_str();

    if(strcmp(ln,"/profile")==0){
        auto p=g_db.get_profile(self->nick);
        if(p.size()<4){self->send_msg("【系统】未找到账号信息\n");return;}
        self->send_msg("PROFILE|"+p[0]+"|"+p[1]+"|"+p[2]+"|"+p[3]+"\n");
        return;}
    if(strncmp(ln,"/profile ",9)==0){
        // 查看他人主页（只读公开信息：头像/昵称/用户名/注册时间）
        char target[64]; strncpy(target, ln+9, sizeof(target)-1); target[sizeof(target)-1]='\0';
        size_t tl=strlen(target);
        while(tl>0&&(target[tl-1]==' '||target[tl-1]=='\t'||target[tl-1]=='\r'||target[tl-1]=='\n'))target[--tl]='\0';
        if(tl==0){self->send_msg("【系统】请输入要查看的用户昵称\n");return;}
        auto p=g_db.get_profile(target);
        if(p.size()<4 || p[1].empty()){self->send_msg("GUEST_PROFILE|__NONE__\n");return;}
        self->send_msg("GUEST_PROFILE|"+p[0]+"|"+p[1]+"|"+p[2]+"|"+p[3]+"\n");
        return;}

    if(strncmp(ln,"/name ",6)==0){
        char nn[64];strncpy(nn,ln+6,sizeof(nn)-1);nn[sizeof(nn)-1]='\0';
        size_t l=strlen(nn);
        while(l>0&&(nn[l-1]==' '||nn[l-1]=='\t'||nn[l-1]=='\r'||nn[l-1]=='\n'))nn[--l]='\0';
        if(l==0){self->send_msg("【系统】昵称不能为空\n");return;}
        char note[256];snprintf(note,sizeof(note),"【系统】%s 改名为 %s\n",self->nick,nn);
        broadcast(note);std::string old(self->nick);
        strncpy(self->nick,nn,sizeof(self->nick)-1);self->nick[sizeof(self->nick)-1]='\0';
        // 改名时同步更新 Redis 在线列表（移除旧昵称，添加新昵称）
        g_redis.user_offline(old);
        g_redis.user_online(self->nick);
        // 单点登录映射同步改名
        auto sit2=sess_map.find(old);
        if(sit2!=sess_map.end()&&sit2->second==self){sess_map.erase(sit2);}
        sess_map[self->nick]=self;
        // 同步更新数据库显示昵称（登录账号不变），保证主页资料实时正确
        g_db.rename_user(old, self->nick);
        self->send_msg("NAME_OK|"+std::string(self->nick)+"\n");
        char tip[128];snprintf(tip,sizeof(tip),"【系统】改名成功！当前昵称：%s\n",self->nick);
        self->send_msg(tip);
        auto it=avatars.find(old);
        if(it!=avatars.end()){std::string u=it->second;avatars.erase(it);avatars[self->nick]=u;
            save_avatars();g_redis.del_avatar(old);g_redis.set_avatar(self->nick,u);g_db.del_avatar(old);g_db.set_avatar(self->nick,u);broadcast_avatar(self->nick,u);}
        return;}

    // ---- 身份 token（断线重连恢复房间） ----
    if(strncmp(ln,"RTOKEN|",7)==0){
        std::string tk=std::string(ln+7);
        size_t e=tk.find_first_of("\r\n");
        if(e!=std::string::npos)tk=tk.substr(0,e);
        if(!tk.empty()){
            self->token=tk;
            // 尝试恢复离线房间
            std::lock_guard<std::mutex> lk(g_room_mtx);
            for(auto&kv:g_rooms){
                GameRoom&r=kv.second;
                if(r.red_token==tk&&r.red==nullptr&&r.red_offline>0){
                    r.red=self;r.red_offline=0;g_user_room[self]=kv.first;
                    std::string tp=std::to_string(r.total_time)+"|"+std::to_string(r.step_time);
                    std::string rs=std::to_string(r.red_ready?1:0)+"|"+std::to_string(r.black_ready?1:0);
                    if(r.started)self->send_msg("GAME|resumed|"+kv.first+"|r|"+tp+"\n");
                    else self->send_msg("GAME|joined|"+kv.first+"|r|"+tp+"|"+rs+"\n");
                    if(r.black)r.black->send_msg("GAME|peer_resumed\n");
                    return;
                }
                if(r.black_token==tk&&r.black==nullptr&&r.black_offline>0){
                    r.black=self;r.black_offline=0;g_user_room[self]=kv.first;
                    std::string tp=std::to_string(r.total_time)+"|"+std::to_string(r.step_time);
                    std::string rs=std::to_string(r.red_ready?1:0)+"|"+std::to_string(r.black_ready?1:0);
                    if(r.started)self->send_msg("GAME|resumed|"+kv.first+"|b|"+tp+"\n");
                    else self->send_msg("GAME|joined|"+kv.first+"|b|"+tp+"|"+rs+"\n");
                    if(r.red)r.red->send_msg("GAME|peer_resumed\n");
                    return;
                }
            }
            // ---- 五子棋房间恢复 ----
            std::lock_guard<std::mutex> lk5(g5_mtx);
            for(auto&kv:g5_rooms){
                G5Room&r=kv.second;
                if(r.b_token==tk&&r.b==nullptr&&r.b_offline>0){
                    r.b=self;r.b_offline=0;g5_user_room[self]=kv.first;
                    std::string rs=std::to_string(r.b_ready?1:0)+"|"+std::to_string(r.w_ready?1:0);
                    if(r.started)self->send_msg("G5|resumed|"+kv.first+"|b"+"\n");
                    else self->send_msg("G5|joined|"+kv.first+"|b|"+rs+"\n");
                    if(r.w)r.w->send_msg("G5|peer_resumed\n");
                    return;
                }
                if(r.w_token==tk&&r.w==nullptr&&r.w_offline>0){
                    r.w=self;r.w_offline=0;g5_user_room[self]=kv.first;
                    std::string rs=std::to_string(r.b_ready?1:0)+"|"+std::to_string(r.w_ready?1:0);
                    if(r.started)self->send_msg("G5|resumed|"+kv.first+"|w"+"\n");
                    else self->send_msg("G5|joined|"+kv.first+"|w|"+rs+"\n");
                    if(r.b)r.b->send_msg("G5|peer_resumed\n");
                    return;
                }
            }
        }
        return;
    }
    // ---- 账号登录（/login 会话token） ----
    if(strncmp(ln,"/login ",7)==0){
        std::string tk=std::string(ln+7);
        size_t e=tk.find_first_of("\r\n");if(e!=std::string::npos)tk=tk.substr(0,e);
        std::string nick=g_db.nick_by_token(tk);
        if(nick.empty()){self->send_msg("LOGIN_BAD|会话无效，请重新登录\n");return;}
        if(g_db.is_banned(nick)){self->send_msg("LOGIN_BAD|账号已被封禁\n");return;}
        // 单点登录：若该账号已在线（另一台设备），先踢掉旧连接
        auto sit=sess_map.find(nick);
        if(sit!=sess_map.end()&&sit->second!=self&&sit->second->fd>=0){
            ChatUser*old=sit->second;
            old->send_msg("KICKED|你的账号已在另一台设备登录\n");
            kill(old);
        }
        sess_map[nick]=self;
        strncpy(self->nick,nick.c_str(),sizeof(self->nick)-1);self->nick[sizeof(self->nick)-1]='\0';
        g_redis.user_online(self->nick);
        g_db.user_join(self->nick);
        self->send_msg("LOGIN_OK|"+nick+"\n");
        if(g_db.user_is_admin(nick))self->send_msg("ADMIN_OK|1\n");
        char note[256];snprintf(note,sizeof(note),"【系统】%s 进入聊天室（当前 %d 人在线）\n",self->nick,online_count());
        broadcast(note);return;}

    if(strncmp(ln,"/join ",6)==0){
        char nm[64];strncpy(nm,ln+6,sizeof(nm)-1);nm[sizeof(nm)-1]='\0';
        size_t l=strlen(nm);
        while(l>0&&(nm[l-1]==' '||nm[l-1]=='\t'||nm[l-1]=='\r'||nm[l-1]=='\n'))nm[--l]='\0';
        if(l==0)return;
        if(g_db.is_banned(nm)){self->send_msg("【系统】账号已被封禁，无法进入\n");return;}
        strncpy(self->nick,nm,sizeof(self->nick)-1);self->nick[sizeof(self->nick)-1]='\0';
        // 用户上线，写入 Redis 和 SQLite
        g_redis.user_online(self->nick);
        g_db.user_join(self->nick);
        char note[256];snprintf(note,sizeof(note),"【系统】%s 进入聊天室（当前 %d 人在线）\n",self->nick,online_count());
        broadcast(note);return;}

    // ---- 管理员命令 ----
    if(strncmp(ln,"/admin ",7)==0||strcmp(ln,"/admin")==0){
        if(!g_db.user_is_admin(self->nick)){self->send_msg("【系统】无管理员权限\n");return;}
        std::string cmd=std::string(ln+7);
        while(!cmd.empty()&&(cmd.back()=='\n'||cmd.back()=='\r'||cmd.back()==' '||cmd.back()=='\t'))cmd.pop_back();
        size_t st0=cmd.find_first_not_of(" \t");if(st0==std::string::npos){self->send_msg("【系统】用法: /admin kick 昵称 | ban 昵称 | unban 昵称 | recall 消息ID | broadcast 内容 | users | clear\n");return;}
        cmd=cmd.substr(st0);
        if(cmd.rfind("kick ",0)==0){
            std::string who=cmd.substr(5);
            ChatUser*t=find_user(who);
            if(!t){self->send_msg("【系统】用户 "+who+" 不在线\n");return;}
            t->send_msg("【系统】你已被管理员踢出聊天室\n");
            std::string note="【系统】管理员已将 "+who+" 踢出聊天室\n";
            broadcast(note);save_hist(note);
            kill(t);
            return;}
        if(cmd.rfind("ban ",0)==0){
            std::string who=cmd.substr(4);
            if(who==self->nick){self->send_msg("【系统】不能封禁自己\n");return;}
            g_db.set_ban(who,1);
            ChatUser*t=find_user(who);
            if(t){t->send_msg("【系统】你已被管理员封禁\n");kill(t);}
            std::string note="【系统】管理员已将 "+who+" 封禁\n";
            broadcast(note);save_hist(note);
            return;}
        if(cmd.rfind("unban ",0)==0){
            std::string who=cmd.substr(6);
            g_db.set_ban(who,0);
            self->send_msg("【系统】已解封 "+who+"\n");
            return;}
        if(cmd.rfind("recall ",0)==0){
            int mid=atoi(cmd.c_str()+7);
            auto it=msg_index.find(mid);
            if(it==msg_index.end()){self->send_msg("【系统】消息不存在或已撤回\n");return;}
            std::string full=it->second.second;
            msg_index.erase(it);
            recent_msg_ids.erase(std::remove(recent_msg_ids.begin(),recent_msg_ids.end(),mid),recent_msg_ids.end());
            for(auto hit=hist.begin();hit!=hist.end();++hit){if(hit->second==full){hist.erase(hit);break;}}
            rewrite_hist();
            std::string proto="RECALL|"+std::to_string(mid)+"|管理员\n";
            broadcast(proto);
            return;}
        if(cmd.rfind("broadcast ",0)==0){
            std::string msg=cmd.substr(10);
            if(msg.empty()){self->send_msg("【系统】公告内容不能为空\n");return;}
            std::string note="【公告】"+msg+"\n";
            broadcast(note);save_hist(note);
            return;}
        if(cmd=="users"){
            auto us=g_db.list_users();
            std::string out="=== 注册用户（"+std::to_string(us.size())+"）===\n";
            for(auto&u:us){
                size_t p1=u.find('|');std::string nm=u.substr(0,p1);
                bool online=find_user(nm)!=nullptr;
                out+="  "+u+"|"+(online?"在线":"离线")+"\n";}
            self->send_msg(out);
            return;}
        if(cmd=="clear"){
            g_db.clear_messages();
            purge_hist();
            broadcast("CLEAR_ALL\n");
            return;}
        self->send_msg("【系统】用法: /admin kick 昵称 | ban 昵称 | unban 昵称 | recall 消息ID | broadcast 内容 | users | clear\n");
        return;}
    // ---- 修改密码（登录用户） ----
    if(strncmp(ln,"/changepwd ",11)==0){
        std::string rest=std::string(ln+11);
        while(!rest.empty()&&(rest.back()=='\n'||rest.back()=='\r'))rest.pop_back();
        size_t sp=rest.find(' ');
        if(sp==std::string::npos||sp+1>=rest.size()){self->send_msg("【系统】格式: /changepwd 旧密码 新密码\n");return;}
        std::string oldp=rest.substr(0,sp),newp=rest.substr(sp+1);
        if(newp.size()<6||newp.size()>32){self->send_msg("【系统】新密码需6-32位\n");return;}
        // 通过显示昵称反查登录账号，用账号做密码校验/修改（防止改名后查不到）
        std::string account=g_db.get_username(self->nick);
        if(account.empty())account=self->nick;
        if(!g_db.verify_pwd(account,oldp)){self->send_msg("【系统】旧密码错误\n");return;}
        if(g_db.change_pwd(account,newp)!=0){self->send_msg("【系统】密码修改失败\n");return;}
        self->send_msg("【系统】密码修改成功，下次请用新密码登录\n");
        return;}

    if(strcmp(ln,"/list")==0){
        // 从内存读取当前连接（最准确），加锁保护
        std::lock_guard<std::mutex> hs_lk(g_hs_mtx);
        std::string out="=== 在线用户（共 "+std::to_string(online_count())+" 人）===\n";
        for(auto&c:hs){auto*u=(ChatUser*)c.get();if(u->is_chat()&&u->fd>=0)
            out+="  "+std::string(u->nick)+" (fd="+std::to_string(u->fd)+")\n";}
        self->send_msg(out);return;}
    if(strncmp(ln,"/recall",7)==0||strncmp(ln,"/撤回",3)==0){
        const char* rest=ln+7;
        while(*rest==' ') rest++;
        if(*rest=='\0') recall_last(self);
        else recall_by_id(self,atoi(rest));
        return;}

    if(strncmp(ln,"/img ",5)==0){
        std::string url=text.substr(5);
        while(!url.empty()&&(url.back()=='\r'||url.back()=='\n'||url.back()==' '))url.pop_back();
        if(url.rfind("/uploads/",0)!=0||url.find("..")!=std::string::npos){
            self->send_msg("【系统】图片地址不合法\n");return;}
        std::string out=std::string(self->nick)+"：[图片]"+url+"\n";
        int mid=record_msg(self->nick,out);
        save_hist(out);broadcast_msg(mid,out);return;}
    if(strncmp(ln,"/file ",6)==0){
        std::string rest=text.substr(6);
        size_t sp=rest.find(' ');
        if(sp==std::string::npos){self->send_msg("【系统】文件格式错误\n");return;}
        std::string url=rest.substr(0,sp);
        std::string fname=rest.substr(sp+1);
        while(!fname.empty()&&(fname.back()=='\r'||fname.back()=='\n'))fname.pop_back();
        if(url.rfind("/uploads/",0)!=0||url.find("..")!=std::string::npos){
            self->send_msg("【系统】文件地址不合法\n");return;}
        std::string out=std::string(self->nick)+"：[文件]"+fname+"|"+url+"\n";
        int mid=record_msg(self->nick,out);
        save_hist(out);broadcast_msg(mid,out);return;}
    if(strncmp(ln,"/video ",7)==0){
        std::string rest=text.substr(7);
        size_t sp=rest.find(' ');
        if(sp==std::string::npos){self->send_msg("【系统】视频格式错误\n");return;}
        std::string url=rest.substr(0,sp);
        std::string vname=rest.substr(sp+1);
        while(!vname.empty()&&(vname.back()=='\r'||vname.back()=='\n'))vname.pop_back();
        if(url.rfind("/uploads/",0)!=0||url.find("..")!=std::string::npos){
            self->send_msg("【系统】视频地址不合法\n");return;}
        std::string out=std::string(self->nick)+"：[视频]"+vname+"|"+url+"\n";
        int mid=record_msg(self->nick,out);
        save_hist(out);broadcast_msg(mid,out);return;}
    if(strncmp(ln,"/audio ",7)==0){
        std::string rest=text.substr(7);
        size_t sp=rest.find(' ');
        if(sp==std::string::npos){self->send_msg("【系统】语音格式错误\n");return;}
        std::string url=rest.substr(0,sp);
        std::string aname=rest.substr(sp+1);
        while(!aname.empty()&&(aname.back()=='\r'||aname.back()=='\n'))aname.pop_back();
        if(url.rfind("/uploads/",0)!=0||url.find("..")!=std::string::npos){
            self->send_msg("【系统】语音地址不合法\n");return;}
        std::string out=std::string(self->nick)+"：[语音]"+aname+"|"+url+"\n";
        int mid=record_msg(self->nick,out);
        save_hist(out);broadcast_msg(mid,out);return;}

    if(strncmp(ln,"/avatar ",8)==0){
        std::string url=text.substr(8);
        while(!url.empty()&&(url.back()=='\r'||url.back()=='\n'||url.back()==' '))url.pop_back();
        if(url=="off"){avatars.erase(self->nick);save_avatars();g_redis.del_avatar(self->nick);g_db.del_avatar(self->nick);broadcast_avatar(self->nick,"");
            self->send_msg("【系统】已恢复默认头像\n");return;}
        if(url.rfind("/uploads/",0)!=0||url.find("..")!=std::string::npos){
            self->send_msg("【系统】头像地址不合法\n");return;}
        avatars[self->nick]=url;save_avatars();g_redis.set_avatar(self->nick,url);g_db.set_avatar(self->nick,url);broadcast_avatar(self->nick,url);
        self->send_msg("【系统】头像已更新\n");return;}

    if(strncmp(ln,"/to ",4)==0){
        const char*rest=ln+4;const char*sp=strchr(rest,' ');
        if(!sp){self->send_msg("【系统】用法: /to 昵称 消息\n");return;}
        std::string tn(rest,sp-rest);std::string msg(sp+1);
        if(msg.empty()||tn==self->nick){
            self->send_msg(tn==self->nick?"【系统】不能私聊自己\n":"【系统】用法: /to 昵称 消息\n");return;}
        ChatUser*t=find_user(tn);
        if(!t){self->send_msg("【系统】用户 "+tn+" 不在线\n");return;}
        {
          std::string dm="【私聊】"+std::string(self->nick)+"："+msg+"\n";
          int mid=record_msg(self->nick,dm);
          t->send_msg("MSGID|"+std::to_string(mid)+"|"+std::to_string((long long)time(nullptr))+"|"+dm);
          std::string echo="【私聊】对 "+tn+"："+msg+"\n";
          int mid2=record_msg(self->nick,echo);
          self->send_msg("MSGID|"+std::to_string(mid2)+"|"+std::to_string((long long)time(nullptr))+"|"+echo);
        }
        return;}

    std::string out=std::string(self->nick)+"："+text+"\n";
    int mid=record_msg(self->nick,out);
    save_hist(out);broadcast_msg(mid,out);}

// ==================== 心跳 ====================
void Reactor::ping_all(){
    time_t now=time(nullptr);
    for(auto&c:hs){auto*u=(ChatUser*)c.get();if(u->is_chat()&&u->fd>=0){
        if(now-u->last_active>60){kill(u);continue;}
        u->send_ping();}}}

class HeartbeatTimer:public Handler{
public:
    Reactor*r;
    bool start(Reactor*r_,int sec){
        r=r_;fd=timerfd_create(CLOCK_MONOTONIC,TFD_NONBLOCK);
        if(fd<0)return false;
        itimerspec ts{};ts.it_value.tv_sec=sec;ts.it_interval.tv_sec=sec;
        timerfd_settime(fd,0,&ts,nullptr);return true;}
    void on_read()override{
        uint64_t exp;while(read(fd,&exp,sizeof(exp))>0){}
        r->ping_all();r->purge_hist();}};

// ==================== WS 客户端 ====================
class WsClient:public ChatUser{
public:
    enum class St{HTTP,WS};
    Reactor*r;std::string root;
    St st=St::HTTP;
    std::string wbuf,rbuf;
    bool processed=false,close_after=false;int status=0;
    size_t body_offset=0,expected_body=0;bool header_done=false;

    WsClient(int c,Reactor*r_,const std::string&rt){fd=c;r=r_;root=rt;rbuf.reserve(WS_BUF);
        snprintf(nick,sizeof(nick),"user%d",fd);}

    void send_raw(const char*d,int l){if(fd<0)return;wbuf.append(d,l);flush();}
    void flush(){
        while(!wbuf.empty()){
            ssize_t n=write(fd,wbuf.data(),wbuf.size());
            if(n>0){wbuf.erase(0,n);continue;}
            if(n<0&&errno==EINTR)continue;
            if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK)){
                if(wbuf.size()>4*1024*1024){r->kill(this);return;}
                r->mod(this,EPOLLIN|EPOLLOUT);return;}
            r->kill(this);return;}
        if(fd>=0){if(close_after)r->kill(this);else r->mod(this,EPOLLIN);}}

    void on_write()override{flush();}
    void ws_frame_send(uint8_t op,const std::string&pl){
        if(fd<0)return;
        char hd[14];int n=0;hd[n++]=0x80|op;size_t l=pl.size();
        if(l<126){hd[n++]=l;}
        else if(l<65536){hd[n++]=126;hd[n++]=(l>>8)&0xFF;hd[n++]=l&0xFF;}
        else{hd[n++]=127;for(int i=7;i>=0;--i)hd[n++]=(l>>(i*8))&0xFF;}
        send_raw(hd,n);if(!pl.empty())send_raw(pl.data(),(int)pl.size());}
    void send_msg(const std::string&s)override{ws_frame_send(0x1,s);}
    void send_ping()override{if(st==St::WS&&fd>=0)ws_frame_send(0x9,"");}
    bool is_chat()override{return st==St::WS;}
    bool is_ws()override{return st==St::WS;}

    void http_resp(int code,const char*txt,const std::string&ct,const std::string&body){
        status=code;char hd[512];
        const char*cc=(ct.find("text/html")!=std::string::npos)?"Cache-Control: no-store, no-cache\r\n":"";
        int n=snprintf(hd,sizeof(hd),
            "HTTP/1.1 %d %s\r\nContent-Type: %s\r\n%s"
            "Content-Length: %zu\r\nConnection: close\r\n\r\n",
            code,txt,ct.c_str(),cc,body.size());
        send_raw(hd,n);if(!body.empty())send_raw(body.data(),(int)body.size());
        close_after=true;}

    void http_resp_file(const std::string&full,const std::string&head){
        struct stat st;if(stat(full.c_str(),&st)!=0){
            http_resp(404,"Not Found","text/html; charset=utf-8",err_page(404,"Not Found"));return;}
        std::string ct=mime_type(full);size_t fsz=(size_t)st.st_size;
        // 根据文件类型设置缓存策略：媒体文件缓存1天，其他不缓存
        std::string cache_hdr;
        if(ct.find("image/")==0||ct.find("video/")==0||ct.find("audio/")==0){
            cache_hdr="Cache-Control: public, max-age=86400\r\n";  // 缓存1天
        }
        size_t rs=0,re=fsz>0?fsz-1:0;bool hasRange=false;
        const char*rp=strstr(head.c_str(),"Range:");
        if(rp){const char*eq=strchr(rp,'=');if(eq){const char*dash=strchr(eq,'-');if(dash){
            rs=(size_t)atoll(eq+1);
            if(*(dash+1)!='\r'&&*(dash+1)!='\n'&&*(dash+1)!='\0')re=(size_t)atoll(dash+1);
            hasRange=true;}}}
        if(fsz>0&&rs>=fsz){char hd[256];int n=snprintf(hd,sizeof(hd),
            "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */%zu\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",fsz);
            send_raw(hd,n);close_after=true;return;}
        if(re>=fsz)re=fsz>0?fsz-1:0;size_t clen=re-rs+1;
        std::ifstream f(full,std::ios::binary);if(!f){http_resp(500,"Internal Server Error","application/json","{\"ok\":false}");return;}
        f.seekg((std::streamoff)rs);std::string body(clen,'\0');f.read(&body[0],(std::streamsize)clen);f.close();
        char hd[512];int n;
        if(hasRange){n=snprintf(hd,sizeof(hd),
            "HTTP/1.1 206 Partial Content\r\nContent-Type: %s\r\nContent-Length: %zu\r\nContent-Range: bytes %zu-%zu/%zu\r\nAccept-Ranges: bytes\r\n%sConnection: close\r\n\r\n",
            ct.c_str(),clen,rs,re,fsz,cache_hdr.c_str());}
        else{n=snprintf(hd,sizeof(hd),
            "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\nAccept-Ranges: bytes\r\n%sConnection: close\r\n\r\n",
            ct.c_str(),fsz,cache_hdr.c_str());}
        send_raw(hd,n);if(!body.empty())send_raw(body.data(),(int)body.size());
        close_after=true;}

    void do_register(const std::string&body){
        const std::string j="application/json; charset=utf-8";
        auto f=parse_form(body);
        std::string u=f["u"],p=f["p"];
        if(u.size()<3||u.size()>20){http_resp(400,"Bad Request",j,"{\"ok\":false,\"msg\":\"用户名需3-20位\"}");return;}
        for(char c:u){if(!(isalnum((unsigned char)c)||c=='_'||c=='-')){
            http_resp(400,"Bad Request",j,"{\"ok\":false,\"msg\":\"用户名只能含字母数字下划线\"}");return;}}
        if(p.size()<6||p.size()>32){http_resp(400,"Bad Request",j,"{\"ok\":false,\"msg\":\"密码需6-32位\"}");return;}
        std::string tk;
        int rc=g_db.register_user(u,p,tk);
        if(rc==1){http_resp(409,"Conflict",j,"{\"ok\":false,\"msg\":\"用户名已被注册\"}");return;}
        if(rc!=0){http_resp(500,"Internal Server Error",j,"{\"ok\":false,\"msg\":\"注册失败\"}");return;}
        http_resp(200,"OK",j,"{\"ok\":true,\"token\":\""+tk+"\",\"nick\":\""+u+"\"}");
    }
    void do_login(const std::string&body){
        const std::string j="application/json; charset=utf-8";
        auto f=parse_form(body);
        std::string u=f["u"],p=f["p"];
        if(u.empty()||p.empty()){http_resp(400,"Bad Request",j,"{\"ok\":false,\"msg\":\"请输入用户名和密码\"}");return;}
        if(g_db.is_banned(u)){http_resp(403,"Forbidden",j,"{\"ok\":false,\"msg\":\"账号已被封禁\"}");return;}
        std::string tk=g_db.login_user(u,p);
        if(tk.empty()){http_resp(401,"Unauthorized",j,"{\"ok\":false,\"msg\":\"用户名或密码错误\"}");return;}
        // 返回数据库中的实际显示昵称（可能已改名，用户名不变）
        std::string display_nick=g_db.nick_by_token(tk);
        if(display_nick.empty())display_nick=u;
        http_resp(200,"OK",j,"{\"ok\":true,\"token\":\""+tk+"\",\"nick\":\""+display_nick+"\"}");
    }
    void do_upload(const std::string&target,const std::string&body){
        std::string ext="bin";size_t q=target.find("ext=");
        if(q!=std::string::npos){ext=target.substr(q+4);
            for(auto&c:ext)c=tolower((unsigned char)c);
            if(ext=="jpeg")ext="jpg";}
        // 过滤危险扩展名
        if(ext=="exe"||ext=="bat"||ext=="sh"||ext=="cmd"||ext=="com"||ext=="vbs"){
            http_resp(400,"Bad Request","application/json","{\"ok\":false,\"msg\":\"不支持的文件类型\"}");return;}
        if(body.empty()||body.size()>MAX_UPLOAD){
            http_resp(413,"Payload Too Large","application/json","{\"ok\":false,\"msg\":\"文件超过10MB\"}");return;}
        static int seq=0;time_t now=time(nullptr);tm t{};localtime_r(&now,&t);
        char name[128];snprintf(name,sizeof(name),"file_%04d%02d%02d_%02d%02d%02d_%d.%s",
            t.tm_year+1900,t.tm_mon+1,t.tm_mday,t.tm_hour,t.tm_min,t.tm_sec,++seq,ext.c_str());
        std::string path=root+"/uploads/"+name;
        std::ofstream f(path,std::ios::binary);
        if(!f){http_resp(500,"Internal Server Error","application/json","{\"ok\":false}");return;}
        f.write(body.data(),(std::streamsize)body.size());
        http_resp(200,"OK","application/json","{\"ok\":true,\"url\":\"/uploads/"+std::string(name)+"\"}");
        std::cout<<"[上传] "<<name<<" "<<body.size()<<"B"<<std::endl;}

    void do_http(const std::string&head,const std::string&body=""){
        size_t eol=head.find("\r\n");if(eol==std::string::npos)eol=head.find('\n');
        std::string line=(eol==std::string::npos)?head:head.substr(0,eol);
        while(!line.empty()&&line.back()=='\r')line.pop_back();
        std::istringstream ss(line);std::string method,target,ver;ss>>method>>target>>ver;
        const std::string html="text/html; charset=utf-8";
        if(method.empty()||target.empty()){http_resp(400,"Bad Request",html,err_page(400,"Bad Request"));}
        else if(method=="POST"&&target.rfind("/api/register",0)==0){do_register(body);}
        else if(method=="POST"&&target.rfind("/api/login",0)==0){do_login(body);}
        else if(method=="POST"&&target.rfind("/upload",0)==0){do_upload(target,body);}
        else if(method!="GET"){http_resp(405,"Method Not Allowed",html,err_page(405,"Method Not Allowed"));}
        else{
            size_t q=target.find('?');if(q!=std::string::npos)target=target.substr(0,q);
            target=url_decode(target);
            if(target.find("..")!=std::string::npos){http_resp(403,"Forbidden",html,err_page(403,"Forbidden"));}
            else{
                std::string rel=target;
                if(rel.empty()||rel=="/")rel="/index.html";
                else if(rel.back()=='/')rel+="index.html";
                std::string full=root+rel;struct stat st;
                if(stat(full.c_str(),&st)==0&&S_ISDIR(st.st_mode))full+="/index.html";
                http_resp_file(full,head);}}
        std::cout<<"[HTTP] "<<line<<" -> "<<status<<std::endl;}

    static bool is_upgrade(const std::string&h){return strstr(h.c_str(),"Upgrade:")&&strstr(h.c_str(),"Sec-WebSocket-Key:");}

    void do_ws_handshake(const std::string&head){
        std::string key;const char*kp=strstr(head.c_str(),"Sec-WebSocket-Key:");
        if(kp){kp+=18;while(*kp==' '||*kp=='\t')++kp;
            const char*end=strstr(kp,"\r\n");if(!end)end=strchr(kp,'\n');
            if(end)key.assign(kp,end-kp);}
        if(key.empty()){processed=true;do_http(head);return;}
        std::string acc=ws_accept_key(key);
        char resp[512];int n=snprintf(resp,sizeof(resp),
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Accept: %s\r\n\r\n",acc.c_str());
        send_raw(resp,n);st=St::WS;
        rbuf.clear();rbuf.shrink_to_fit();rbuf.reserve(WS_BUF);
        std::cout<<"[WS] handshake OK"<<std::endl;
        r->send_avatars(this);r->send_hist(this);}

    bool handle_frame(uint8_t op,const std::string&pl){
        if(op==0x1){r->handle_msg(this,pl);return fd>=0;}
        if(op==0x8){ws_frame_send(0x8,pl);r->kill(this);return false;}
        if(op==0x9){ws_frame_send(0xA,pl);return fd>=0;}return true;}

    void process_frames(){
        while(rbuf.size()>0){
            uint8_t op=0;std::string pl;
            int n=ws_frame((const unsigned char*)rbuf.data(),rbuf.size(),op,pl);
            if(n<0){ws_frame_send(0x8,"");r->kill(this);return;}
            if(n==0)break;
            rbuf.erase(0,n);
            if(!handle_frame(op,pl))return;}}

    void on_read()override{
        if(st==St::HTTP&&processed){r->kill(this);return;}
        size_t cap=(st==St::HTTP)?(size_t)(MAX_UPLOAD+65536):(size_t)WS_BUF;
        if(rbuf.size()>=cap){r->kill(this);return;}
        size_t old=rbuf.size();rbuf.resize(cap);
        ssize_t n=read(fd,&rbuf[old],cap-old);
        if(n<0){rbuf.resize(old);
            if(errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK)return;
            r->kill(this);return;}
        if(n==0){rbuf.resize(old);r->kill(this);return;}
        rbuf.resize(old+n);
        last_active=time(nullptr);

        if(st==St::HTTP){
            if(!header_done){
                size_t end=0;
                size_t p1=rbuf.find("\r\n\r\n");
                if(p1!=std::string::npos)end=p1+4;
                else{size_t p2=rbuf.find("\n\n");if(p2!=std::string::npos)end=p2+2;}
                if(end==0){if(rbuf.size()>=MAX_HEADER){http_resp(400,"Bad Request","text/html",err_page(400,"Bad Request"));
                    processed=true;}return;}
                std::string hd(rbuf,0,end);
                if(is_upgrade(hd)){
                    do_ws_handshake(hd);
                    rbuf.erase(0,end);
                    if(fd>=0&&!rbuf.empty())process_frames();
                    return;}
                std::string ml=hd.substr(0,hd.find("\r\n"));
                bool is_post=ml.rfind("POST",0)==0;
                expected_body=is_post?parse_content_length(hd):0;
                if(expected_body>MAX_UPLOAD){http_resp(413,"Payload Too Large","application/json","{\"ok\":false}");
                    processed=true;return;}
                body_offset=end;header_done=true;}
            if(header_done){
                size_t total=body_offset+expected_body;
                if(rbuf.size()<total)return;
                std::string bd(expected_body>0?rbuf.substr(body_offset,expected_body):"");
                processed=true;
                do_http(std::string(rbuf,0,body_offset),bd);}}
        else process_frames();}};

// ==================== Telnet 客户端 ====================
class TelnetClient:public ChatUser{
public:
    Reactor*r;std::string wbuf;char rbuf[REQ_BUF]={};int rlen=0;
    TelnetClient(int c,Reactor*r_):r(r_){
        fd=c;snprintf(nick,sizeof(nick),"user%d",fd);
        static const char W[]="=== 欢迎进入聊天室（telnet）===\n"
            "浏览器用户也会在这里，消息互通\n"
            "输入 /name 昵称 修改名字\n"
            "输入 /list 查看在线用户\n"
            "输入 /to 昵称 消息 私聊\n"
            "输入 /quit 退出\n";
        send_raw(W,sizeof(W)-1);}
    void send_raw(const char*d,int l){if(fd<0)return;wbuf.append(d,l);flush();}
    void send_msg(const std::string&s)override{send_raw(s.data(),(int)s.size());}
    bool is_chat()override{return true;}
    void flush(){
        while(!wbuf.empty()){
            ssize_t n=write(fd,wbuf.data(),wbuf.size());
            if(n>0){wbuf.erase(0,n);continue;}
            if(n<0&&errno==EINTR)continue;
            if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK)){
                if(wbuf.size()>4*1024*1024){r->kill(this);return;}
                r->mod(this,EPOLLIN|EPOLLOUT);return;}
            r->kill(this);return;}
        if(fd>=0)r->mod(this,EPOLLIN);}
    void on_write()override{flush();}
    void on_read()override{
        if(rlen>=sizeof(rbuf)-1){r->kill(this);return;}
        ssize_t n=read(fd,rbuf+rlen,sizeof(rbuf)-1-rlen);
        if(n<0){if(errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK)return;r->kill(this);return;}
        if(n==0){r->kill(this);return;}rlen+=n;last_active=time(nullptr);
        int w=0;
        for(int i=0;i<rlen;++i){
            if((unsigned char)rbuf[i]==0xFF&&i+2<rlen){i+=2;continue;}
            rbuf[w++]=rbuf[i];}
        rlen=w;rbuf[rlen]='\0';
        while(true){
            char*p=(char*)memchr(rbuf,'\n',rlen);if(!p)break;
            int ml=p-rbuf+1;std::string line(rbuf,ml);
            while(!line.empty()&&(line.back()=='\n'||line.back()=='\r'))line.pop_back();
            if(line=="/quit"||line=="/exit"){send_msg("再见！\n");r->kill(this);return;}
            r->handle_msg(this,line);
            int rem=rlen-ml;memmove(rbuf,rbuf+ml,rem);rlen=rem;}}};

// ==================== TCP Listener ====================
class Listener:public Handler{
public:
    Reactor*r;
    bool listen_on(int port){
        fd=socket(AF_INET,SOCK_STREAM,0);
        if(fd<0)return false;
        int opt=1;setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
        sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(port);a.sin_addr.s_addr=INADDR_ANY;
        if(bind(fd,(sockaddr*)&a,sizeof(a))<0){close(fd);fd=-1;return false;}
        if(listen(fd,128)<0){close(fd);fd=-1;return false;}
        set_nonblocking(fd);return true;}};

class WsAcceptor:public Listener{
public:
    std::string root;
    WsAcceptor(Reactor*r_,int port,const std::string&rt):root(rt){r=r_;listen_on(port);}
    void on_read()override{
        sockaddr_in ci{};socklen_t cl=sizeof(ci);
        int c=accept(fd,(sockaddr*)&ci,&cl);
        if(c<0){if(errno!=EAGAIN&&errno!=EWOULDBLOCK)perror("accept");return;}
        if(r->conn_count>=MAX_CONN){std::cerr<<"[reject] max connections"<<std::endl;close(c);return;}
        set_nonblocking(c);
        auto p=std::make_unique<WsClient>(c,r,root);
        if(!r->reg(p.get(),EPOLLIN|EPOLLRDHUP)){close(c);return;}
        r->conn_count++;{std::lock_guard<std::mutex> hl(g_hs_mtx);r->hs.push_back(std::move(p));}}};

class TelnetAcceptor:public Listener{
public:
    TelnetAcceptor(Reactor*r_,int port){r=r_;listen_on(port);}
    void on_read()override{
        sockaddr_in ci{};socklen_t cl=sizeof(ci);
        int c=accept(fd,(sockaddr*)&ci,&cl);
        if(c<0){if(errno!=EAGAIN&&errno!=EWOULDBLOCK)perror("accept");return;}
        if(r->conn_count>=MAX_CONN){std::cerr<<"[reject] max connections"<<std::endl;close(c);return;}
        set_nonblocking(c);
        auto p=std::make_unique<TelnetClient>(c,r);
        if(!r->reg(p.get(),EPOLLIN|EPOLLRDHUP)){close(c);return;}
        ChatUser* tc=p.get();
        r->conn_count++;{std::lock_guard<std::mutex> hl(g_hs_mtx);r->hs.push_back(std::move(p));}
        r->send_hist(tc);
        std::cout<<"[TELNET] fd="<<c<<" nick="<<tc->nick<<std::endl;}};

// ==================== main ====================
// 广播线程入口
static void broadcast_worker(Reactor* reactor){
    while(g_running){
        reactor->do_broadcast_batch();
    }
    // 退出前处理剩余消息
    {std::lock_guard<std::mutex> lk(g_bcast_mtx);
    if(g_bcast_q.empty())return;}
    reactor->do_broadcast_batch();
}

int main(int argc,char*argv[]){
    signal(SIGPIPE,SIG_IGN);
    signal(SIGINT,sig_handler);
    signal(SIGTERM,sig_handler);

    std::string root=(argc>1)?argv[1]:"www";
    while(root.size()>1&&root.back()=='/')root.pop_back();
    mkdir(root.c_str(),0755);
    mkdir((root+"/uploads").c_str(),0755);

    std::string idx=root+"/index.html";struct stat st;
    if(stat(idx.c_str(),&st)!=0){
        std::ofstream f(idx);
        f<<"<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>Chat Server</title></head>"
          "<body style=\"font-family:sans-serif;text-align:center;padding:3rem\">"
          "<h1>WebSocket Chat Server</h1>"
          "<p><a href=\"/chat.html\">进入聊天室</a></p></body></html>\n";}

    Reactor reactor;
    // 连接 Redis（失败时降级为内存缓存）
    g_redis.connect();
    // 连接 SQLite（持久化存储）
    g_db.connect("chatroom.db");
    // 从 SQLite 加载头像到内存
    {auto avs=g_db.get_all_avatars();for(auto&kv:avs)reactor.avatars[kv.first]=kv.second;}

    if(!reactor.init()){std::cerr<<"epoll 失败\n";return 1;}
    reactor.load_hist("chat_history.log");
    reactor.load_avatars("chat_avatars.log");

    // 端口：./server <root> [port]，默认 8888（生产部署可显式指定）
    int port = (argc > 2) ? atoi(argv[2]) : 8888;
    if (port < 1 || port > 65535) port = 8888;

    WsAcceptor wa(&reactor, port, root);
    if(wa.fd<0){std::cerr<<port<<" 端口失败\n";return 1;}
    reactor.reg(&wa,EPOLLIN);

    TelnetAcceptor ta(&reactor, port + 1);
    if(ta.fd<0){std::cerr<<(port + 1)<<" 端口失败\n";return 1;}
    reactor.reg(&ta,EPOLLIN);

    HeartbeatTimer hb;
    if(hb.start(&reactor,30))reactor.reg(&hb,EPOLLIN);

    std::cout<<"=== 聊天服务器启动 ===\n"
             <<"浏览器:http://<服务器IP>:"<<port<<"/chat.html\n"
             
             <<"最大连接: "<<MAX_CONN<<"\n"
             <<"Ctrl+C 停止\n";

    // 启动广播线程
    std::thread bcast_thread(broadcast_worker, &reactor);

    reactor.run();

    // 退出清理
    g_running = false;
    g_bcast_cv.notify_all();
    if(bcast_thread.joinable()) bcast_thread.join();

    std::cout<<"\n服务器关闭"<<std::endl;
    reactor.hs.clear();
    return 0;}