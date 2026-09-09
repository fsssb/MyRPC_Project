#ifndef MYRPCPROJECT_INCLUDE_REGISTRYPROTOCOL_H_
#define MYRPCPROJECT_INCLUDE_REGISTRYPROTOCOL_H_

#include <cstdint>

// Wire contract between RegistryClient and rpc_registry_server: method keys and
// Serializer field ids (see RegistryClient.cpp / rpc_registry_server.cpp).
namespace registry_proto {

constexpr char kRegister[] = "registry.register";
constexpr char kUnregister[] = "registry.unregister";
constexpr char kRenew[] = "registry.renew";
constexpr char kLookup[] = "registry.lookup";
constexpr char kWatch[] = "registry.watch";

constexpr uint32_t kFService = 1;
constexpr uint32_t kFHost = 2;
constexpr uint32_t kFPort = 3;
constexpr uint32_t kFInstanceId = 4;
constexpr uint32_t kFLeaseMs = 5;
constexpr uint32_t kFInstances = 6;  // lookup response: array of instance structs

}  // namespace registry_proto

#endif  // MYRPCPROJECT_INCLUDE_REGISTRYPROTOCOL_H_
