#include "source/extensions/filters/http/ext_authz/config.h"

#include <chrono>
#include <string>

#include "envoy/config/core/v3/grpc_service.pb.h"
#include "envoy/extensions/filters/http/ext_authz/v3/ext_authz.pb.h"
#include "envoy/extensions/filters/http/ext_authz/v3/ext_authz.pb.validate.h"
#include "envoy/grpc/async_client_manager.h"
#include "envoy/registry/registry.h"

#include "source/common/common/utility.h"
#include "source/common/config/utility.h"
#include "source/common/protobuf/utility.h"
#include "source/extensions/filters/common/ext_authz/ext_authz_grpc_impl.h"
#include "source/extensions/filters/common/ext_authz/ext_authz_http_impl.h"
#include "source/extensions/filters/http/ext_authz/ext_authz.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace ExtAuthz {

Http::FilterFactoryCb ExtAuthzFilterFactory::createFilterFactoryFromProtoTyped(
    const envoy::extensions::filters::http::ext_authz::v3::ExtAuthz& proto_config,
    const std::string& stats_prefix, DualInfo dual_info,
    Server::Configuration::ServerFactoryContext& context) {
  if (dual_info.is_upstream_filter) {
    if (proto_config.clear_route_cache()) {
      ExceptionUtil::throwEnvoyException(
          "clear_route_cache cannot be enabled on an upstream ext_authz filter.");
    }
    if (proto_config.include_peer_certificate()) {
      ExceptionUtil::throwEnvoyException(
          "include_peer_certificate cannot be enabled on an upstream ext_authz filter.");
    }
    if (proto_config.include_tls_session()) {
      ExceptionUtil::throwEnvoyException(
          "include_tls_session cannot be enabled on an upstream ext_auth filter.");
    }
  }
  const auto filter_config =
      std::make_shared<FilterConfig>(proto_config, dual_info.scope, context.runtime(),
                                     context.httpContext(), stats_prefix, context.bootstrap());
  // The callback is created in main thread and executed in worker thread, variables except factory
  // context must be captured by value into the callback.
  Http::FilterFactoryCb callback;

  if (proto_config.has_http_service()) {
    // Raw HTTP client.
    const uint32_t timeout_ms = PROTOBUF_GET_MS_OR_DEFAULT(proto_config.http_service().server_uri(),
                                                           timeout, DefaultTimeout);
    const auto client_config =
        std::make_shared<Extensions::Filters::Common::ExtAuthz::ClientConfig>(
            proto_config, timeout_ms, proto_config.http_service().path_prefix());
    callback = [filter_config, client_config,
                &context](Http::FilterChainFactoryCallbacks& callbacks) {
      auto client = std::make_unique<Extensions::Filters::Common::ExtAuthz::RawHttpClientImpl>(
          context.clusterManager(), client_config);
      callbacks.addStreamFilter(std::make_shared<Filter>(filter_config, std::move(client)));
    };
  } else {
    // gRPC client.
    const uint32_t timeout_ms =
        PROTOBUF_GET_MS_OR_DEFAULT(proto_config.grpc_service(), timeout, DefaultTimeout);

    THROW_IF_NOT_OK(Config::Utility::checkTransportVersion(proto_config));
    Envoy::Grpc::GrpcServiceConfigWithHashKey config_with_hash_key =
        Envoy::Grpc::GrpcServiceConfigWithHashKey(proto_config.grpc_service());
    callback = [&context, filter_config, timeout_ms, config_with_hash_key,
                dual_info](Http::FilterChainFactoryCallbacks& callbacks) {
      auto client = std::make_unique<Filters::Common::ExtAuthz::GrpcClientImpl>(
          context.clusterManager().grpcAsyncClientManager().getOrCreateRawAsyncClientWithHashKey(
              config_with_hash_key, dual_info.scope, true),
          std::chrono::milliseconds(timeout_ms));
      callbacks.addStreamFilter(std::make_shared<Filter>(filter_config, std::move(client)));
    };
  }

  return callback;
}

Router::RouteSpecificFilterConfigConstSharedPtr
ExtAuthzFilterFactory::createRouteSpecificFilterConfigTyped(
    const envoy::extensions::filters::http::ext_authz::v3::ExtAuthzPerRoute& proto_config,
    Server::Configuration::ServerFactoryContext&, ProtobufMessage::ValidationVisitor&) {
  return std::make_shared<FilterConfigPerRoute>(proto_config);
}

/**
 * Static registration for the external authorization filter. @see RegisterFactory.
 */
LEGACY_REGISTER_FACTORY(ExtAuthzFilterFactory, Server::Configuration::NamedHttpFilterConfigFactory,
                        "envoy.ext_authz");
LEGACY_REGISTER_FACTORY(UpstreamExtAuthzFilterFactory,
                        Server::Configuration::UpstreamHttpFilterConfigFactory, "envoy.ext_authz");

} // namespace ExtAuthz
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
