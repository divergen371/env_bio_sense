#pragma once
#include <ESPAsyncWebServer.h>
#include "storage/storage_manager.h"
#include <memory>

#include "services/archive_manager.h"
#include "services/location_service.h"
#include "services/ppg_session_manager.h"

namespace services {

class WebServerService {
public:
    WebServerService(storage::StorageManager& storageManager,
                     ArchiveManager& archiveManager,
                     LocationService& locationService,
                     PpgSessionManager& ppgSessionManager);
    void begin();

private:
    storage::StorageManager& storageManager_;
    ArchiveManager& archiveManager_;
    LocationService& locationService_;
    PpgSessionManager& ppgSessionManager_;
    std::unique_ptr<AsyncWebServer> server_;
    char csrfToken_[33] {};

    void setupRoutes();
    bool authorize(AsyncWebServerRequest* request) const;
    bool authorizeMutation(AsyncWebServerRequest* request) const;
};

} // namespace services
