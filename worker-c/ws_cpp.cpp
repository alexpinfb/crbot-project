#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/client.hpp>

#include <hiredis/hiredis.h>

#include <iostream>
#include <cstdlib>
#include <string>

extern "C" void parse_event(const char *msg);
extern "C" {
#include "http_keepalive.h"
}

typedef websocketpp::client<websocketpp::config::asio_tls_client> client;

static std::string envv(const char *k) {
    const char *v = std::getenv(k);
    return v ? std::string(v) : std::string();
}

static void redis_heartbeat() {
    std::string addr = envv("REDIS_ADDR");
    std::string port_s = envv("REDIS_PORT");
    std::string pass = envv("REDIS_PASSWORD");
    std::string worker = envv("WORKER_ID");
    std::string account = envv("ACCOUNT_ID");

    if (addr.empty() || pass.empty() || worker.empty()) {
        std::cerr << "CPP_HEARTBEAT_SKIP missing redis env\n";
        return;
    }

    int port = port_s.empty() ? 6379 : std::atoi(port_s.c_str());

    redisContext *rc = redisConnect(addr.c_str(), port);

    if (!rc || rc->err) {
        std::cerr << "CPP_HEARTBEAT_CONNECT_FAIL\n";
        if (rc) redisFree(rc);
        return;
    }

    redisReply *auth = (redisReply *)redisCommand(rc, "AUTH %s", pass.c_str());
    if (auth) freeReplyObject(auth);

    std::string key = "crbot:workerInfo:" + worker;

    std::string json =
        std::string("{") +
        "\"workerId\":\"" + worker + "\"," +
        "\"worker_id\":\"" + worker + "\"," +
        "\"accountId\":\"" + account + "\"," +
        "\"account_id\":\"" + account + "\"," +
        "\"instance\":\"cpp-readonly\"," +
        "\"online\":true," +
        "\"provider\":\"cpp\"" +
        "}";

    redisReply *r = (redisReply *)redisCommand(
        rc,
        "SETEX %s 30 %s",
        key.c_str(),
        json.c_str()
    );

    if (r) {
        std::cout << "CPP_HEARTBEAT_OK " << key << "\n";
        freeReplyObject(r);
    }

    redisFree(rc);
}

int main() {
    std::string cookie = envv("CRBOT_COOKIE");
    std::string ua = envv("CRBOT_UA");

    if (cookie.empty() || ua.empty()) {
        std::cerr << "missing CRBOT_COOKIE or CRBOT_UA\n";
        return 1;
    }

    client c;
    c.clear_access_channels(websocketpp::log::alevel::all);
    c.clear_error_channels(websocketpp::log::elevel::all);
    c.init_asio();

    c.set_tls_init_handler([](websocketpp::connection_hdl) {
        auto ctx = websocketpp::lib::make_shared<boost::asio::ssl::context>(
            boost::asio::ssl::context::tlsv12_client
        );
        ctx->set_verify_mode(boost::asio::ssl::verify_none);
        return ctx;
    });

    c.set_open_handler([&](websocketpp::connection_hdl) {
        std::cout << "CPP_WS_READY\n";
        redis_heartbeat();
            keepalive_warmup_once();
    });

    c.set_message_handler([&](websocketpp::connection_hdl hdl, client::message_ptr msg) {
        std::string p = msg->get_payload();

        std::cout << "CPP_WS_RECV " << p << "\n";

        if (p == "2") {
            c.send(hdl, "3", websocketpp::frame::opcode::text);
            std::cout << "CPP_WS_SEND 3\n";
            redis_heartbeat();
            keepalive_warmup_once();
            return;
        }

        if (!p.empty() && p[0] == '0') {
            c.send(hdl, "40", websocketpp::frame::opcode::text);
            std::cout << "CPP_WS_SEND 40\n";
            return;
        }

        if (p.rfind("40", 0) == 0) {
            c.send(hdl, "42[\"list:initialize\"]", websocketpp::frame::opcode::text);
            std::cout << "CPP_WS_SEND list:initialize\n";
            return;
        }

        if (p.find("42[\"list:update\"") != std::string::npos) {
            parse_event(p.c_str());
        }
    });

    websocketpp::lib::error_code ec;
    client::connection_ptr con = c.get_connection(
        "wss://app.send.tg/internal/v1/p2c-socket/?EIO=4&transport=websocket",
        ec
    );

    if (ec) {
        std::cerr << "get_connection error: " << ec.message() << "\n";
        return 1;
    }

    con->append_header("Cookie", cookie);
    con->append_header("Origin", "https://app.send.tg");
    con->append_header("User-Agent", ua);

    c.connect(con);
    c.run();

    return 0;
}
