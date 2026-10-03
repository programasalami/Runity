#include <doctest/doctest.h>

#include <atomic>
#include <random>

#include <asio.hpp>

#include "waw/core/sha256.hpp"
#include "waw/core/task_queue.hpp"
#include "waw/persistence/account_sessions.hpp"
#include "waw/persistence/redis_client.hpp"

using namespace waw;
using namespace std::chrono_literals;
using persistence::RedisReply;

TEST_CASE("sha256 matches the FIPS 180-4 test vectors") {
    CHECK(core::Sha256::hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(core::Sha256::hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(core::Sha256::hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    core::Sha256 million;
    const std::string chunk(1000, 'a');
    for (int i = 0; i < 1000; ++i) million.update(chunk);
    const auto digest = million.finish();
    CHECK(digest[0] == 0xcd);
    CHECK(digest[31] == 0xd0);  // cdc76e5c...c7112cd0
}

TEST_CASE("RESP replies parse, including partial input") {
    persistence::RespParser p;
    p.feed("+OK\r\n-ERR bad\r\n:42\r\n$5\r\nhel");
    auto a = p.next();
    REQUIRE(a.has_value());
    REQUIRE(a->has_value());
    CHECK((*a)->type == RedisReply::Type::Status);
    CHECK((*a)->text == "OK");
    auto b = p.next();
    CHECK((*b)->type == RedisReply::Type::Error);
    auto c = p.next();
    CHECK((*c)->integer == 42);
    auto d = p.next();
    REQUIRE(d.has_value());
    CHECK_FALSE(d->has_value());  // bulk incomplete
    p.feed("lo\r\n$-1\r\n*2\r\n$1\r\na\r\n*1\r\n:7\r\n");
    auto e = p.next();
    CHECK((*e)->text == "hello");
    auto f = p.next();
    CHECK((*f)->is_nil());
    auto g = p.next();
    REQUIRE((*g)->type == RedisReply::Type::Array);
    CHECK((*g)->elements.size() == 2);
    CHECK((*g)->elements[1].elements[0].integer == 7);
}

TEST_CASE("RESP garbage is an error") {
    persistence::RespParser p;
    p.feed("?what\r\n");
    CHECK_FALSE(p.next().has_value());
    persistence::RespParser q;
    q.feed(":12x\r\n");
    CHECK_FALSE(q.next().has_value());
}

TEST_CASE("commands are encoded as RESP bulk arrays") {
    CHECK(persistence::encode_command({"GET", "k"}) == "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n");
}

TEST_CASE("redis URLs parse") {
    auto e = persistence::RedisEndpoint::parse("redis://127.0.0.1:6380");
    REQUIRE(e.has_value());
    CHECK(e->port == 6380);
    CHECK(persistence::RedisEndpoint::parse("redis://host")->port == 6379);
    CHECK_FALSE(persistence::RedisEndpoint::parse("http://x").has_value());
    CHECK_FALSE(persistence::RedisEndpoint::parse("redis://x:0").has_value());
}

TEST_CASE("the task queue runs posted tasks on the draining thread only") {
    core::TaskQueue q;
    int ran = 0;
    std::jthread poster([&] {
        for (int i = 0; i < 100; ++i) q.post([&ran] { ++ran; });
    });
    poster.join();
    CHECK(ran == 0);
    CHECK(q.run_pending() == 100);
    CHECK(ran == 100);
}

TEST_CASE("the worker finishes every submitted job before it is destroyed") {
    std::atomic<int> done{0};
    {
        core::Worker w;
        for (int i = 0; i < 50; ++i) {
            w.submit([&done] {
                std::this_thread::sleep_for(1ms);
                ++done;
            });
        }
    }
    CHECK(done.load() == 50);
}

TEST_CASE("the in-memory session store enforces single use and single owner") {
    persistence::InMemoryAccountSessions s;
    s.add_ticket("t1", {.account_id = 5, .name = "Bob", .rank = 0});
    auto id = s.redeem_join_ticket("t1");
    REQUIRE(id.has_value());
    CHECK(id->name == "Bob");
    CHECK(s.redeem_join_ticket("t1").error() == persistence::SessionError::InvalidTicket);
    CHECK(s.acquire_lock(5, "a", 30s).has_value());
    CHECK(s.acquire_lock(5, "b", 30s).error() == persistence::SessionError::AccountInUse);
    CHECK_FALSE(*s.refresh_lock(5, "b", 30s));
    CHECK(s.release_lock(5, "b").has_value());
    CHECK(s.is_locked(5));
    CHECK(s.release_lock(5, "a").has_value());
    CHECK_FALSE(s.is_locked(5));
}

namespace {

bool local_redis_available() {
    static const bool available = [] {
        asio::io_context io;
        asio::ip::tcp::socket socket(io);
        asio::error_code ec;
        socket.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 6379), ec);
        return !ec;
    }();
    return available;
}

std::string unique_prefix() {
    std::random_device rd;
    return "waw:test:" + std::to_string(rd()) + std::to_string(rd()) + ":";
}

}  // namespace

TEST_CASE("redis account sessions follow the key contract shared with the Account/API service") {
    if (!local_redis_available()) {
        MESSAGE("skipped: no Redis on 127.0.0.1:6379");
        return;
    }
    const auto prefix = unique_prefix();
    persistence::RedisClient admin({"127.0.0.1", 6379});
    persistence::RedisAccountSessions sessions(std::make_unique<persistence::RedisClient>(persistence::RedisEndpoint{"127.0.0.1", 6379}),
                                               prefix);

    // The C# side writes {prefix}join:{sha256 hex} -> {"accountId":..,"name":..,"rank":..}; the same test vector is in its tests.
    CHECK(sessions.join_key("abc") == prefix + "join:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    REQUIRE(admin.command({"SET", sessions.join_key("ticket-1"), R"({"accountId":9,"name":"Amy","rank":90})", "EX", "60"}).has_value());

    auto identity = sessions.redeem_join_ticket("ticket-1");
    REQUIRE(identity.has_value());
    CHECK(identity->account_id == 9);
    CHECK(identity->name == "Amy");
    CHECK(identity->rank == 90);
    CHECK(sessions.redeem_join_ticket("ticket-1").error() == persistence::SessionError::InvalidTicket);
    CHECK(sessions.redeem_join_ticket("never-issued").error() == persistence::SessionError::InvalidTicket);

    CHECK(sessions.acquire_lock(9, "server-a/1", 5000ms).has_value());
    CHECK(sessions.acquire_lock(9, "server-a/2", 5000ms).error() == persistence::SessionError::AccountInUse);
    CHECK(*sessions.refresh_lock(9, "server-a/1", 5000ms));
    CHECK_FALSE(*sessions.refresh_lock(9, "server-a/2", 5000ms));
    CHECK(sessions.release_lock(9, "server-a/2").has_value());
    auto still = admin.command({"GET", sessions.lock_key(9)});
    REQUIRE(still.has_value());
    CHECK(still->text == "server-a/1");
    CHECK(sessions.release_lock(9, "server-a/1").has_value());
    CHECK(admin.command({"EXISTS", sessions.lock_key(9)})->integer == 0);

    // A lock left by a crashed server expires by itself.
    CHECK(sessions.acquire_lock(10, "crashed/1", 100ms).has_value());
    std::this_thread::sleep_for(250ms);
    CHECK(sessions.acquire_lock(10, "server-b/1", 5000ms).has_value());
    (void)sessions.release_lock(10, "server-b/1");
}

TEST_CASE("an unreachable redis fails closed and quickly") {
    persistence::RedisAccountSessions sessions(
        std::make_unique<persistence::RedisClient>(persistence::RedisEndpoint{"127.0.0.1", 1}, 300ms), "waw:test:");
    const auto start = std::chrono::steady_clock::now();
    CHECK(sessions.redeem_join_ticket("x").error() == persistence::SessionError::ServiceUnavailable);
    CHECK(sessions.acquire_lock(1, "o", 1000ms).error() == persistence::SessionError::ServiceUnavailable);
    CHECK(std::chrono::steady_clock::now() - start < 3s);
}
