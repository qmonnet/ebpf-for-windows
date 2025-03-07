// Copyright (c) Microsoft Corporation
// SPDX-License-Identifier: MIT

// This module facilitates testing various connect redirect scenarios by sending traffic to both
// local system and a remote system, both running TCP / UDP listeners.

#define CATCH_CONFIG_RUNNER

#include "catch_wrapper.hpp"

#include "bpf/bpf.h"
#pragma warning(push)
#pragma warning(disable : 4200)
#include "bpf/libbpf.h"
#pragma warning(pop)
#include "common_tests.h"
#include "ebpf_nethooks.h"
#include "ebpf_structs.h"
#include "socket_helper.h"
#include "socket_tests_common.h"

#include <mstcpip.h>

static std::string _family;
static std::string _protocol;
static std::string _vip_v4;
static std::string _vip_v6;
static std::string _local_ip_v4;
static std::string _local_ip_v6;
static std::string _remote_ip_v4;
static std::string _remote_ip_v6;
static uint16_t _destination_port = 4444;
static uint16_t _proxy_port = 4443;

typedef struct _test_addresses
{
    struct sockaddr_storage loopback_address;
    struct sockaddr_storage remote_address;
    struct sockaddr_storage local_address;
    struct sockaddr_storage vip_address;
} test_addresses_t;

typedef struct _test_globals
{
    ADDRESS_FAMILY family;
    IPPROTO protocol;
    uint16_t destination_port;
    uint16_t proxy_port;
    test_addresses_t addresses[socket_family_t::Max];
} test_globals_t;

static test_globals_t _globals;
static volatile bool _globals_initialized = false;

inline static IPPROTO
_get_protocol_from_string(std::string protocol)
{
    if (protocol.compare("udp") == 0 || protocol.compare("UDP") == 0) {
        return IPPROTO_UDP;
    } else if (protocol.compare("tcp") == 0 || protocol.compare("TCP") == 0) {
        return IPPROTO_TCP;
    }

    REQUIRE(false);
}

static void
_initialize_test_globals()
{
    if (_globals_initialized) {
        return;
    }

    ADDRESS_FAMILY family;
    uint32_t v4_addresses = 0;
    uint32_t v6_addresses = 0;

    // Read v4 addresses.
    if (_remote_ip_v4 != "") {
        get_address_from_string(
            _remote_ip_v4, _globals.addresses[socket_family_t::IPv4].remote_address, false, &family);
        REQUIRE(family == AF_INET);
        get_address_from_string(_remote_ip_v4, _globals.addresses[socket_family_t::Dual].remote_address, true, &family);
        REQUIRE(family == AF_INET);
        v4_addresses++;
    }
    if (_local_ip_v4 != "") {
        get_address_from_string(_local_ip_v4, _globals.addresses[socket_family_t::IPv4].local_address, false, &family);
        REQUIRE(family == AF_INET);
        get_address_from_string(_local_ip_v4, _globals.addresses[socket_family_t::Dual].local_address, true, &family);
        REQUIRE(family == AF_INET);
        v4_addresses++;
    }
    if (_vip_v4 != "") {
        get_address_from_string(_vip_v4, _globals.addresses[socket_family_t::IPv4].vip_address, false, &family);
        REQUIRE(family == AF_INET);
        get_address_from_string(_vip_v4, _globals.addresses[socket_family_t::Dual].vip_address, true, &family);
        REQUIRE(family == AF_INET);
        v4_addresses++;
    }
    REQUIRE((v4_addresses == 0 || v4_addresses == 3));

    IN4ADDR_SETLOOPBACK((PSOCKADDR_IN)&_globals.addresses[socket_family_t::IPv4].loopback_address);
    IN6ADDR_SETV4MAPPED(
        (PSOCKADDR_IN6)&_globals.addresses[socket_family_t::Dual].loopback_address,
        &in4addr_loopback,
        scopeid_unspecified,
        0);

    // Read v6 addresses.
    if (_remote_ip_v6 != "") {
        get_address_from_string(
            _remote_ip_v6, _globals.addresses[socket_family_t::IPv6].remote_address, false, &family);
        REQUIRE(family == AF_INET6);
        v6_addresses++;
    }
    if (_local_ip_v6 != "") {
        get_address_from_string(_local_ip_v6, _globals.addresses[socket_family_t::IPv6].local_address, false, &family);
        REQUIRE(family == AF_INET6);
        v6_addresses++;
    }
    if (_vip_v6 != "") {
        get_address_from_string(_vip_v6, _globals.addresses[socket_family_t::IPv6].vip_address, false, &family);
        REQUIRE(family == AF_INET6);
        v6_addresses++;
    }
    REQUIRE((v6_addresses == 0 || v6_addresses == 3));
    IN6ADDR_SETLOOPBACK((PSOCKADDR_IN6)&_globals.addresses[socket_family_t::IPv6].loopback_address);

    _globals.destination_port = _destination_port;
    _globals.proxy_port = _proxy_port;
    _globals_initialized = true;
}

static void
_load_and_attach_ebpf_programs(_Outptr_ struct bpf_object** return_object)
{
    struct bpf_object* object = bpf_object__open("cgroup_sock_addr2.o");
    REQUIRE(object != nullptr);
    REQUIRE(bpf_object__load(object) == 0);

    bpf_program* connect_program_v4 = bpf_object__find_program_by_name(object, "connect_redirect4");
    REQUIRE(connect_program_v4 != nullptr);

    int result = bpf_prog_attach(
        bpf_program__fd(const_cast<const bpf_program*>(connect_program_v4)), 0, BPF_CGROUP_INET4_CONNECT, 0);
    REQUIRE(result == 0);

    bpf_program* connect_program_v6 = bpf_object__find_program_by_name(object, "connect_redirect6");
    REQUIRE(connect_program_v6 != nullptr);

    result = bpf_prog_attach(
        bpf_program__fd(const_cast<const bpf_program*>(connect_program_v6)), 0, BPF_CGROUP_INET6_CONNECT, 0);
    REQUIRE(result == 0);

    *return_object = object;
}

static void
_update_policy_map(
    _In_ const struct bpf_object* object,
    _In_ sockaddr_storage& destination,
    _In_ sockaddr_storage& proxy,
    uint16_t destination_port,
    uint16_t proxy_port,
    uint32_t protocol,
    bool dual_stack,
    bool add)
{
    bpf_map* policy_map = bpf_object__find_map_by_name(object, "policy_map");
    REQUIRE(policy_map != nullptr);

    fd_t map_fd = bpf_map__fd(policy_map);

    // Insert / delete redirect policy entry in the map.
    destination_entry_t key = {0};
    destination_entry_t value = {0};

    if (_globals.family == AF_INET && dual_stack) {
        struct sockaddr_in6* v6_destination = (struct sockaddr_in6*)&destination;
        struct sockaddr_in6* v6_proxy = (struct sockaddr_in6*)&proxy;

        INET_SET_ADDRESS(
            AF_INET6, (PUCHAR)&key.destination_ip, IN6_GET_ADDR_V4MAPPED((IN6_ADDR*)&v6_destination->sin6_addr));
        INET_SET_ADDRESS(
            AF_INET6, (PUCHAR)&value.destination_ip, IN6_GET_ADDR_V4MAPPED((IN6_ADDR*)&v6_proxy->sin6_addr));
    } else {
        INET_SET_ADDRESS(_globals.family, (PUCHAR)&key.destination_ip, INETADDR_ADDRESS((PSOCKADDR)&destination));
        INET_SET_ADDRESS(_globals.family, (PUCHAR)&value.destination_ip, INETADDR_ADDRESS((PSOCKADDR)&proxy));
    }

    key.destination_port = htons(destination_port);
    value.destination_port = htons(proxy_port);
    key.protocol = protocol;

    if (add) {
        REQUIRE(bpf_map_update_elem(map_fd, &key, &value, 0) == 0);
    } else {
        REQUIRE(bpf_map_delete_elem(map_fd, &key) == 0);
    }
}

void
connect_redirect_test(
    _In_ const struct bpf_object* object,
    _In_ client_socket_t* sender_socket,
    _In_ sockaddr_storage& destination,
    _In_ sockaddr_storage& proxy,
    uint16_t destination_port,
    uint16_t proxy_port,
    bool dual_stack)
{
    bool add_policy = true;
    // Update policy in the map to redirect the connection to the proxy.
    _update_policy_map(
        object, destination, proxy, destination_port, proxy_port, _globals.protocol, dual_stack, add_policy);

    // Try to send and receive message to "destination". It should succeed.
    sender_socket->send_message_to_remote_host(CLIENT_MESSAGE, destination, _globals.destination_port);
    sender_socket->complete_async_send(1000, expected_result_t::SUCCESS);

    sender_socket->post_async_receive();
    sender_socket->complete_async_receive(2000, false);

    uint32_t bytes_received = 0;
    char* received_message = nullptr;
    sender_socket->get_received_message(bytes_received, received_message);

    std::string expected_response = SERVER_MESSAGE + std::to_string(proxy_port);
    REQUIRE(strlen(received_message) == strlen(expected_response.c_str()));
    REQUIRE(memcmp(received_message, expected_response.c_str(), strlen(received_message)) == 0);

    // Remove entry from policy map.
    add_policy = false;
    _update_policy_map(
        object, destination, proxy, destination_port, proxy_port, _globals.protocol, dual_stack, add_policy);
}

void
authorize_test(
    _In_ const struct bpf_object* object,
    _In_ client_socket_t* sender_socket,
    _Inout_ sockaddr_storage& destination,
    bool dual_stack)
{
    // Default behavior of the eBPF program is to block the connection.

    // Send should fail as the connection is blocked.
    sender_socket->send_message_to_remote_host(CLIENT_MESSAGE, destination, _globals.destination_port);
    sender_socket->complete_async_send(1000, expected_result_t::FAILURE);

    // Receive should timeout as connection is blocked.
    sender_socket->post_async_receive(true);
    sender_socket->complete_async_receive(1000, true);

    // Now update the policy map to allow the connection and test again.
    connect_redirect_test(
        object,
        sender_socket,
        destination,
        destination,
        _globals.destination_port,
        _globals.destination_port,
        dual_stack);
}

void
get_client_socket(bool dual_stack, _Inout_ client_socket_t** sender_socket)
{
    client_socket_t* old_socket = *sender_socket;
    client_socket_t* new_socket = nullptr;
    socket_family_t family = dual_stack
                                 ? socket_family_t::Dual
                                 : ((_globals.family == AF_INET) ? socket_family_t::IPv4 : socket_family_t::IPv6);
    if (_globals.protocol == IPPROTO_TCP) {
        new_socket = (client_socket_t*)new stream_client_socket_t(SOCK_STREAM, IPPROTO_TCP, 0, family);
    } else {
        new_socket = (client_socket_t*)new datagram_client_socket_t(SOCK_DGRAM, IPPROTO_UDP, 0, family);
    }

    *sender_socket = new_socket;
    if (old_socket) {
        delete old_socket;
    }
}

void
authorize_test_wrapper(_In_ const struct bpf_object* object, bool dual_stack, _In_ sockaddr_storage& destination)
{
    client_socket_t* sender_socket = nullptr;

    get_client_socket(dual_stack, &sender_socket);
    authorize_test(object, sender_socket, destination, dual_stack);
    delete sender_socket;
}

void
connect_redirect_test_wrapper(
    _In_ const struct bpf_object* object,
    _In_ sockaddr_storage& destination,
    _In_ sockaddr_storage& proxy,
    bool dual_stack)
{
    client_socket_t* sender_socket = nullptr;
    get_client_socket(dual_stack, &sender_socket);
    connect_redirect_test(
        object, sender_socket, destination, proxy, _globals.destination_port, _globals.proxy_port, dual_stack);
    delete sender_socket;
}

void
connect_redirect_tests_common(_In_ const struct bpf_object* object, bool dual_stack, _In_ test_addresses_t& addresses)
{
    // First category is authorize tests.
    const char* protocol_string = (_globals.protocol == IPPROTO_TCP) ? "TCP" : "UDP";
    const char* family_string = (_globals.family == AF_INET) ? "IPv4" : "IPv6";
    const char* dual_stack_string = dual_stack ? "Dual Stack" : "No Dual Stack";

    // Loopback address.
    printf("AUTH: Loopback | %s | %s | %s\n", protocol_string, family_string, dual_stack_string);
    authorize_test_wrapper(object, dual_stack, addresses.loopback_address);

    // Remote address.
    printf("AUTH: Remote | %s | %s | %s\n", protocol_string, family_string, dual_stack_string);
    authorize_test_wrapper(object, dual_stack, addresses.remote_address);

    // Local non-loopback address.
    printf("AUTH: Local | %s | %s | %s\n", protocol_string, family_string, dual_stack_string);
    authorize_test_wrapper(object, dual_stack, addresses.local_address);

    // Second category is connection redirection tests.

    // Remote -> Remote
    printf("REDIRECT: Remote -> Remote | %s | %s | %s\n", protocol_string, family_string, dual_stack_string);
    connect_redirect_test_wrapper(object, addresses.vip_address, addresses.remote_address, dual_stack);

    // Remote -> Loopback
    printf("REDIRECT: Remote -> Loopback | %s | %s | %s\n", protocol_string, family_string, dual_stack_string);
    connect_redirect_test_wrapper(object, addresses.vip_address, addresses.loopback_address, dual_stack);

    // Remote -> Local non-loopback
    printf("REDIRECT: Remote -> Local | %s | %s | %s\n", protocol_string, family_string, dual_stack_string);
    connect_redirect_test_wrapper(object, addresses.vip_address, addresses.local_address, dual_stack);

    // Loopback -> Remote
    printf("REDIRECT: Loopback -> Remote | %s | %s | %s\n", protocol_string, family_string, dual_stack_string);
    connect_redirect_test_wrapper(object, addresses.loopback_address, addresses.remote_address, dual_stack);

    // Loopback -> Local non-loopback
    printf("REDIRECT: Loopback -> Local | %s | %s | %s\n", protocol_string, family_string, dual_stack_string);
    connect_redirect_test_wrapper(object, addresses.loopback_address, addresses.local_address, dual_stack);

    // Local non-loopback -> Loopback
    printf("REDIRECT: Local -> Loopback | %s | %s | %s\n", protocol_string, family_string, dual_stack_string);
    connect_redirect_test_wrapper(object, addresses.local_address, addresses.loopback_address, dual_stack);

    // Local non-loopback -> Remote
    printf("REDIRECT: Local -> Remote | %s | %s | %s\n", protocol_string, family_string, dual_stack_string);
    connect_redirect_test_wrapper(object, addresses.local_address, addresses.remote_address, dual_stack);
}

void
test_common(ADDRESS_FAMILY family, IPPROTO protocol)
{
    _initialize_test_globals();

    struct bpf_object* object = nullptr;
    _load_and_attach_ebpf_programs(&object);

    _globals.family = family;
    _globals.protocol = protocol;
    socket_family_t socket_family = (family == AF_INET) ? socket_family_t::IPv4 : socket_family_t::IPv6;
    socket_family_t dual_stack_socket_family = (family == AF_INET) ? socket_family_t::Dual : socket_family_t::IPv6;

    connect_redirect_tests_common(object, false, _globals.addresses[socket_family]);
    connect_redirect_tests_common(object, true, _globals.addresses[dual_stack_socket_family]);

    // This should also detach the programs as they are not pinned.
    bpf_object__close(object);
}

TEST_CASE("connect_redirect_tcp_v4", "[connect_redirect_tests]") { test_common(AF_INET, IPPROTO_TCP); }

TEST_CASE("connect_redirect_tcp_v6", "[connect_redirect_tests]") { test_common(AF_INET6, IPPROTO_TCP); }

TEST_CASE("connect_redirect_udp_v4", "[connect_redirect_tests]") { test_common(AF_INET, IPPROTO_UDP); }

TEST_CASE("connect_redirect_udp_v6", "[connect_redirect_tests]") { test_common(AF_INET6, IPPROTO_UDP); }

int
main(int argc, char* argv[])
{
    Catch::Session session;

    // Use Catch's composite command line parser.
    using namespace Catch::Clara;
    auto cli = session.cli() | Opt(_vip_v4, "v4 Virtual / Load Balanced IP")["-v"]["--virtual-ip-v4"]("IPv4 VIP") |
               Opt(_vip_v6, "v6 Virtual / Load Balanced IP")["-v"]["--virtual-ip-v6"]("IPv6 VIP") |
               Opt(_local_ip_v4, "v4 local IP")["-l"]["--local-ip-v4"]("Local IPv4 IP") |
               Opt(_local_ip_v6, "v6 local IP")["-l"]["--local-ip-v6"]("Local IPv6 IP") |
               Opt(_remote_ip_v4, "v4 Remote IP")["-r"]["--remote-ip-v4"]("IPv4 Remote IP") |
               Opt(_remote_ip_v6, "v6 Remote IP")["-r"]["--remote-ip-v6"]("IPv6 Remote IP") |
               Opt(_destination_port, "Destination Port")["-t"]["--destination-port"]("Destination Port") |
               Opt(_proxy_port, "Proxy Port")["-pt"]["--proxy-port"]("Proxy Port");

    session.cli(cli);

    int returnCode = session.applyCommandLine(argc, argv);
    if (returnCode != 0)
        return returnCode;

    WSAData data;

    int error = WSAStartup(2, &data);
    if (error != 0) {
        printf("Unable to load Winsock: %d\n", error);
        return 1;
    }

    session.run();
    WSACleanup();
}
