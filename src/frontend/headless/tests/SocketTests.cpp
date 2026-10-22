/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include "TestHarness.h"
#include "Socket.h"

using namespace Headless::Net;

TEST(socket_port_zero_picks_nonzero_port)
{
    uint16_t port = 0;
    Socket s = BindFirstFree(0, 1, port);
    CHECK(s != InvalidSocket);
    CHECK(port != 0);
    CloseSocket(s);
}

TEST(socket_next_port_when_taken)
{
    // base comes from the OS, so base + 1 may not be bindable at all: another process holds it, or it is in a Windows
    // excluded port range (Hyper-V reserves blocks of 100 inside the dynamic range). Probe base + 1 first and retry
    // with a new base when it is unavailable; a BindFirstFree that does not move on fails every attempt.
    bool tested = false;
    for (int attempt = 0; attempt < 20 && !tested; attempt++)
    {
        uint16_t base = 0;
        Socket a = BindFirstFree(0, 1, base);
        CHECK(a != InvalidSocket);
        if (a == InvalidSocket)
            return;
        uint16_t probed = 0;
        Socket probe = base < 65535 ? BindFirstFree(base + 1, 1, probed) : InvalidSocket;
        if (probe == InvalidSocket)
        {
            CloseSocket(a);
            continue;
        }
        CloseSocket(probe);
        uint16_t second = 0;
        Socket b = BindFirstFree(base, 8, second);
        CHECK(b != InvalidSocket);
        CHECK_EQ(second, base + 1);
        if (b != InvalidSocket)
            CloseSocket(b);
        CloseSocket(a);
        tested = true;
    }
    CHECK(tested);
}

TEST(socket_strict_range_fails_when_taken)
{
    uint16_t base = 0;
    Socket a = BindFirstFree(0, 1, base);
    CHECK(a != InvalidSocket);
    if (a == InvalidSocket)
        return;
    uint16_t other = 1234;
    Socket b = BindFirstFree(base, 1, other);
    CHECK(b == InvalidSocket);
    CloseSocket(a);
}

TEST(socket_port_reusable_after_close)
{
    uint16_t base = 0;
    Socket a = BindFirstFree(0, 1, base);
    CHECK(a != InvalidSocket);
    CloseSocket(a);
    uint16_t again = 0;
    Socket b = BindFirstFree(base, 1, again);
    CHECK(b != InvalidSocket);
    CHECK_EQ(again, base);
    CloseSocket(b);
}

TEST(socket_listens_on_loopback_only)
{
    uint16_t port = 0;
    Socket s = BindFirstFree(0, 1, port);
    CHECK(s != InvalidSocket);
    CHECK(LocalAddress(s) == 0x7F000001u);
    CloseSocket(s);
}

TEST(socket_connect_send_recv)
{
    uint16_t port = 0;
    Socket l = BindFirstFree(0, 1, port);
    CHECK(l != InvalidSocket);
    Socket c = ConnectLoopback(port);
    CHECK(c != InvalidSocket);
    CHECK(WaitReadable(l, 2000));
    Socket a = Accept(l);
    CHECK(a != InvalidSocket);

    const uint8_t msg[5] = {1, 2, 3, 4, 5};
    CHECK(SendAll(c, msg, 5));
    CHECK(WaitReadable(a, 2000));
    uint8_t buf[16] = {};
    int n = Recv(a, buf, sizeof buf);
    CHECK_EQ(n, 5);
    CHECK(std::memcmp(buf, msg, 5) == 0);

    // nothing pending: not readable within a short wait
    CHECK(!WaitReadable(a, 10));

    // peer close -> Recv returns 0
    CloseSocket(c);
    CHECK(WaitReadable(a, 2000));
    CHECK_EQ(Recv(a, buf, sizeof buf), 0);
    CloseSocket(a);
    CloseSocket(l);
}

TEST(socket_send_all_large)
{
    uint16_t port = 0;
    Socket l = BindFirstFree(0, 1, port);
    Socket c = ConnectLoopback(port);
    WaitReadable(l, 2000);
    Socket a = Accept(l);
    CHECK(a != InvalidSocket);
    std::vector<uint8_t> big(4 * 1024 * 1024);
    for (size_t i = 0; i < big.size(); i++)
        big[i] = uint8_t(i * 7);
    bool sent = false;
    std::thread t([&] { sent = SendAll(c, big.data(), big.size()); });
    std::vector<uint8_t> got;
    uint8_t buf[65536];
    while (got.size() < big.size())
    {
        int n = Recv(a, buf, sizeof buf);
        if (n <= 0)
            break;
        got.insert(got.end(), buf, buf + n);
    }
    t.join();
    CHECK(sent);
    CHECK(got == big);
    CloseSocket(a);
    CloseSocket(c);
    CloseSocket(l);
}

TEST(socket_send_reports_would_block_on_full_buffer)
{
    uint16_t port = 0;
    Socket listener = BindFirstFree(0, 1, port);
    CHECK(listener != InvalidSocket);
    Socket client = ConnectLoopback(port);
    CHECK(client != InvalidSocket);
    Socket server = Accept(listener);
    CHECK(server != InvalidSocket);
    CHECK(SetNonBlocking(server, true));
    std::vector<uint8_t> chunk(1 << 16, 0x5A);
    size_t total = 0;
    bool blocked = false;
    for (int i = 0; i < 4096 && !blocked; i++)      // the peer never reads
    {
        int n = Send(server, chunk.data(), chunk.size());
        if (n > 0)
            total += size_t(n);
        else
        {
            CHECK(n < 0);
            CHECK(WouldBlock());
            blocked = true;
        }
    }
    CHECK(blocked);
    CHECK(total > 0);
    CloseSocket(server);
    CloseSocket(client);
    CloseSocket(listener);
}

TEST(socket_parse_ipv4)
{
    uint32_t ip = 0;
    CHECK(ParseIPv4("127.0.0.1", ip));
    CHECK(ip == 0x7F000001u);
    CHECK(ParseIPv4("0.0.0.0", ip));
    CHECK(ip == 0u);
    CHECK(ParseIPv4("192.168.1.20", ip));
    CHECK(ip == 0xC0A80114u);
    CHECK(ParseIPv4("localhost", ip));
    CHECK(ip == 0x7F000001u);
    CHECK(!ParseIPv4("", ip));
    CHECK(!ParseIPv4("256.0.0.1", ip));
    CHECK(!ParseIPv4("1.2.3", ip));
    CHECK(!ParseIPv4("1.2.3.4.5", ip));
    CHECK(!ParseIPv4("1.2.3.4 ", ip));
    CHECK(!ParseIPv4("example.com", ip));
}

TEST(socket_binds_the_requested_address)
{
    uint16_t port = 0;
    Socket s = BindFirstFree(0, 1, port, 0);     // 0.0.0.0: every interface
    CHECK(s != InvalidSocket);
    CHECK(LocalAddress(s) == 0u);
    Socket c = ConnectLoopback(port);            // loopback is one of them
    CHECK(c != InvalidSocket);
    CloseSocket(c);
    CloseSocket(s);
}
