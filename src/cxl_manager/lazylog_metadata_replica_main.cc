#include "cxl_manager/lazylog_metadata_replica.h"

#include <iostream>
#include <string>

#include <cxxopts.hpp>

// A sequencing-metadata replica is intentionally a separate process from the
// global binder.  This keeps metadata durability independent of binding
// availability and lets an experiment place the two replica sets separately.
int main(int argc, char** argv) {
  cxxopts::Options options("lazylog_metadata_replica",
                           "Durable LazyLog sequencing-metadata replica");
  options.add_options()
      ("listen", "gRPC listen address", cxxopts::value<std::string>()->default_value("0.0.0.0:50081"))
      ("sidecar", "Durable metadata sidecar path", cxxopts::value<std::string>()->default_value("lazylog-metadata.sidecar"))
      ("h,help", "Show usage");

  const auto args = options.parse(argc, argv);
  if (args.count("help")) {
    std::cout << options.help() << '\n';
    return 0;
  }

  LazyLog::LazyLogMetadataReplicaServer server(
      args["listen"].as<std::string>(), args["sidecar"].as<std::string>());
  std::string error;
  if (!server.Start(&error)) {
    std::cerr << "lazylog_metadata_replica: " << error << '\n';
    return 1;
  }
  server.Wait();
  return 0;
}
