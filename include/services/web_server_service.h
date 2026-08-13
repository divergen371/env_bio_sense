#pragma once
#include <ESPAsyncWebServer.h>
#include "storage/storage_manager.h"
#include <memory>

namespace services {

class WebServerService {
public:
    WebServerService(storage::StorageManager& storageManager);
    void begin();

private:
    storage::StorageManager& storageManager_;
    std::unique_ptr<AsyncWebServer> server_;

    void setupRoutes();
};

} // namespace services
