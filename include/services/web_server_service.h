#pragma once
#include <ESPAsyncWebServer.h>
#include "storage/storage_manager.h"
#include <memory>

#include "services/archive_manager.h"
#include "services/location_service.h"

namespace services {

class WebServerService {
public:
    WebServerService(storage::StorageManager& storageManager,
                     ArchiveManager& archiveManager,
                     LocationService& locationService);
    void begin();

private:
    storage::StorageManager& storageManager_;
    ArchiveManager& archiveManager_;
    LocationService& locationService_;
    std::unique_ptr<AsyncWebServer> server_;

    void setupRoutes();
};

} // namespace services
