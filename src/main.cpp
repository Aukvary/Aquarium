#include "./Users.hpp"
#include "./Utils.hpp"
#include "userver/ugrpc/client/client_factory_component.hpp"

#include <userver/clients/dns/component.hpp>
#include <userver/components/minimal_server_component_list.hpp>
#include <userver/congestion_control/component.hpp>
#include <userver/storages/postgres/component.hpp>
#include <userver/testsuite/testsuite_support.hpp>
#include <userver/ugrpc/client/component_list.hpp>
#include <userver/ugrpc/server/component_list.hpp>
#include <userver/utest/using_namespace_userver.hpp>
#include <userver/utils/daemon_run.hpp>

int main(int argc, char* argv[]) {
    const auto component_list =
        components::MinimalServerComponentList()
            .Append<components::TestsuiteSupport>()
            .Append<ugrpc::client::ClientFactoryComponent>()
            .AppendComponentList(ugrpc::client::MinimalComponentList())
            .AppendComponentList(ugrpc::server::MinimalComponentList())
            .Append<userver::clients::dns::Component>()
            .Append<userver::components::Postgres>("postgres-db")
            .Append<Aquarium::Utils::UtilsServiceComponent>()
            .Append<Aquarium::Users::UserStoreClientComponent>()
            .Append<Aquarium::Users::UserStoreService>();
    return utils::DaemonMain(argc, argv, component_list);
}
