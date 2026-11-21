#include "zenz_scorer/posix_runtime.h"

#if defined(_WIN32)
#error "posix_runtime.cc must not be built on Windows"
#endif

#include <memory>
#include <string>
#include <utility>

#include "zenz/zenz_wire_protocol.h"
#include "zenz_scorer/llama_backend.h"
#include "zenz_scorer/posix_server.h"

namespace mozc {
namespace zenz_scorer {
namespace {

constexpr int kMinimumNPredict = 4;
constexpr int kMaximumNPredict = 256;

PosixZenzResponse ChildUnavailableResponse() {
  PosixZenzResponse response;
  response.status = ::mozc::zenz::kZenzWireStatusError;
  response.debug = "llama_server_not_running";
  return response;
}

}  // namespace

PosixScorerRuntime::PosixScorerRuntime(PosixScorerRuntimeOptions options)
    : options_(std::move(options)) {}

PosixScorerRuntime::~PosixScorerRuntime() { Stop(); }

bool PosixScorerRuntime::Start(std::string* error) {
  if (error == nullptr) {
    return false;
  }
  error->clear();

  if (server_ != nullptr || llama_process_ != nullptr) {
    *error = "already_started";
    return false;
  }
  if (options_.n_predict < kMinimumNPredict ||
      options_.n_predict > kMaximumNPredict) {
    *error = "invalid_n_predict";
    return false;
  }

  if (!LaunchLlamaServer(requested_backend_device_, error)) {
    return false;
  }

  server_ = std::make_unique<PosixZenzScorerServer>(
      options_.socket_path,
      [this](const PosixZenzRequest& request) { return Handle(request); });

  if (!server_->Start(error)) {
    server_.reset();
    llama_process_.reset();
    http_backend_ = nullptr;
    return false;
  }
  return true;
}

bool PosixScorerRuntime::LaunchLlamaServer(const std::string& backend_device,
                                           std::string* error) {
  // Stop the current child first: only one model is resident at a time.
  llama_process_.reset();
  http_backend_ = nullptr;

  LlamaServerProcessOptions process_options = options_.llama_server;
  if (!backend_device.empty()) {
    process_options.additional_args.push_back("--device");
    process_options.additional_args.push_back(backend_device);
  }
  auto process =
      std::make_unique<LlamaServerProcess>(std::move(process_options));
  if (!process->Start(error)) {
    return false;
  }

  LlamaBackendOptions backend_options;
  backend_options.port = process->port();
  backend_options.api_key = process->api_key();
  backend_options.n_predict = options_.n_predict;
  http_backend_ = CreateLlamaHttpBackend(std::move(backend_options));
  llama_process_ = std::move(process);
  return true;
}

PosixZenzResponse PosixScorerRuntime::Handle(
    const PosixZenzRequest& request) {
  if (request.backend_device != requested_backend_device_) {
    requested_backend_device_ = request.backend_device;
    std::string error;
    if (!LaunchLlamaServer(requested_backend_device_, &error) &&
        !requested_backend_device_.empty()) {
      // The device is missing or cannot load the model; keep Zenz usable
      // with automatic selection.
      LaunchLlamaServer(std::string(), &error);
    }
  }
  if (llama_process_ == nullptr || !http_backend_ ||
      !llama_process_->running()) {
    return ChildUnavailableResponse();
  }
  return http_backend_(request);
}

bool PosixScorerRuntime::ServeOne(int timeout_msec, std::string* error) {
  if (error == nullptr) {
    return false;
  }
  error->clear();

  if (server_ == nullptr) {
    *error = "not_started";
    return false;
  }
  if (llama_process_ == nullptr || !llama_process_->running()) {
    *error = "llama_server_exited";
    return false;
  }
  return server_->ServeOne(timeout_msec, error);
}

void PosixScorerRuntime::Stop() {
  // Remove the public-facing socket before stopping the child so no new
  // requests can enter while the localhost backend is shutting down.
  server_.reset();
  llama_process_.reset();
  http_backend_ = nullptr;
}

bool PosixScorerRuntime::running() {
  return server_ != nullptr && llama_process_ != nullptr &&
         llama_process_->running();
}

}  // namespace zenz_scorer
}  // namespace mozc
