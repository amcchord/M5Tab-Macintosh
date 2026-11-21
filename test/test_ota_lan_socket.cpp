// Run the legacy receiver against the released Python sender over loopback.
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

#define lwip_socket socket
#define lwip_fcntl fcntl
#define lwip_connect connect
#define lwip_select select
#define lwip_getsockopt getsockopt
#define lwip_send send
#define lwip_recv recv
#define lwip_close close
#include "../src/basilisk/ota_http_lan.h"

int main(int argc, char **argv) {
    assert(argc == 3);
    std::ifstream file(argv[2], std::ios::binary);
    const std::vector<char> expected{std::istreambuf_iterator<char>(file), {}};
    assert(expected.size() >= 3 * 1024 * 1024);
    sockaddr_in peer = {};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(std::atoi(argv[1]));
    assert(inet_pton(AF_INET, "127.0.0.1", &peer.sin_addr) == 1);
    OtaHttpLan http;
    assert(http.open(peer));
    auto await = [](auto step) {
        auto result = step();
        while (result == OtaHttpLan::Result::Pending) {
            usleep(1000);
            result = step();
        }
        assert(result == OtaHttpLan::Result::Done);
    };
    await([&] { return http.sendRequest(); });
    await([&] { return http.headers(expected.size()); });
    size_t pos = 0, positive_reads = 0;
    char buf[4096];
    while (pos < expected.size()) {
        const int n = http.read(buf, std::min(sizeof(buf), expected.size() - pos));
        if (n < 0 && (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR)) {
            usleep(1000);
            continue;
        }
        assert(n > 0 && !memcmp(buf, expected.data() + pos, n));
        pos += n;
        ++positive_reads;
    }
    // A regression to a transport read for each byte is always observable.
    assert(positive_reads < expected.size() / 128);
}
