// Run the sync daemon or an on-demand device scan.
#include "clipsync/daemon.hpp"
#include "clipsync/discovery_net.hpp"
#include "clipsync/peer_store.hpp"

#include <cstdio>
#include <cstring>

int main(int argc, char **argv) {
    if (argc == 3 && std::strcmp(argv[1], "add-peer") == 0) {
        return clipsync::add_peer_address(argv[2]);
    }
    if (argc == 2 && std::strcmp(argv[1], "scan") == 0) {
        return clipsync::scan_devices();
    }
    if (argc > 2 || (argc == 2 && std::strcmp(argv[1], "daemon") != 0)) {
        std::fprintf(stderr, "usage: clipsyncd [daemon|scan|add-peer IPv4]\n");
        return 2;
    }
    return clipsync::run();
}
