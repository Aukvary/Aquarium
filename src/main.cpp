#include <userver/clients/dns/component.hpp>
#include <userver/components/minimal_server_component_list.hpp>
#include <userver/congestion_control/component.hpp>
#include <userver/testsuite/testsuite_support.hpp>
#include <userver/ugrpc/server/component_list.hpp>
#include <userver/utest/using_namespace_userver.hpp>
#include <userver/utils/daemon_run.hpp>

int main(int argc, char* argv[]) {
    const auto component_list =
        components::MinimalServerComponentList().Append<>();

    return utils::DaemonMain(argc, argv, component_list);
}
