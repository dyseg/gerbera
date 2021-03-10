/*GRB*

    Gerbera - https://gerbera.io/
    
    upnp_quirks.cc - this file is part of Gerbera.
    
    Copyright (C) 2020-2021 Gerbera Contributors
    
    Gerbera is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License version 2
    as published by the Free Software Foundation.

    Gerbera is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Gerbera.  If not, see <http://www.gnu.org/licenses/>.

    $Id$
*/

/// \file upnp_quirks.cc

#include "upnp_quirks.h" // API

#include <unistd.h>

#include "cds_objects.h"
#include "config/config_manager.h"
#include "content/content_manager.h"
#include "request_handler.h"
#include "server.h"
#include "util/tools.h"
#include "util/upnp_clients.h"
#include "util/upnp_headers.h"

Quirks::Quirks(std::shared_ptr<Context> context, const struct sockaddr_storage* addr, const std::string& userAgent)
    : context(std::move(context))
    , content(this->context->getServer()->getContent())
{
    this->context->getClients()->getInfo(addr, userAgent, &pClientInfo);
}

void Quirks::addCaptionInfo(const std::shared_ptr<CdsItem>& item, std::unique_ptr<Headers>& headers) const
{
    if ((pClientInfo->flags & QUIRK_FLAG_SAMSUNG) == 0)
        return;

    if (!startswith(item->getMimeType(), "video"))
        return;

    std::string url;
    if (!UpnpXMLBuilder::renderSubtitle(context->getServer()->getVirtualUrl(), item, url)) {
        constexpr auto exts = std::array {
            ".srt",
            ".ssa",
            ".smi",
            ".sub"
        };

        // remove .ext
        fs::path path = item->getLocation();
        std::string pathNoExt = path.parent_path() / path.stem();

        auto it = std::find_if(exts.begin(), exts.end(), [=](const auto& ext) { std::string captionPath = pathNoExt + ext;
              return access(captionPath.c_str(), R_OK) == 0; });

        if (it == exts.end())
            return;

        url = context->getServer()->getVirtualUrl() + RequestHandler::joinUrl({ CONTENT_MEDIA_HANDLER, URL_OBJECT_ID, fmt::to_string(item->getID()), URL_RESOURCE_ID, "0", URL_FILE_EXTENSION, "file" + fmt::to_string(*it) });
    }
    headers->addHeader("CaptionInfo.sec", url);
}

void Quirks::restoreSamsungBookMarkedPosition(const std::shared_ptr<CdsItem>& item, pugi::xml_node* result) const
{
    if ((pClientInfo->flags & QUIRK_FLAG_SAMSUNG_BOOKMARK_SEC) == 0 && (pClientInfo->flags & QUIRK_FLAG_SAMSUNG_BOOKMARK_MSEC) == 0)
        return;
    auto positionToRestore = item->getBookMarkPos();
    if (positionToRestore > 10)
        positionToRestore -= 10;
    log_info("restoreSamsungBookMarkedPosition: Title [{}] positionToRestore [{}] sec", item->getTitle(), positionToRestore);

    if (pClientInfo->flags & QUIRK_FLAG_SAMSUNG_BOOKMARK_MSEC)
        positionToRestore *= 1000;

    auto dcmInfo = fmt::format("CREATIONDATE=0,FOLDER={},BM={}", item->getTitle(), positionToRestore);
    result->append_child("sec:dcmInfo").append_child(pugi::node_pcdata).set_value(dcmInfo.c_str());
}

void Quirks::saveSamsungBookMarkedPosition(const std::unique_ptr<ActionRequest>& request) const
{
    if ((pClientInfo->flags & QUIRK_FLAG_SAMSUNG_BOOKMARK_SEC) == 0 && (pClientInfo->flags & QUIRK_FLAG_SAMSUNG_BOOKMARK_MSEC) == 0) {
        log_debug("saveSamsungBookMarkedPosition called, but it is not enabled for this client");
    } else {
        auto divider = (pClientInfo->flags & QUIRK_FLAG_SAMSUNG_BOOKMARK_MSEC) == 0 ? 1 : 1000;
        auto req_root = request->getRequest()->document_element();
        auto objectID = req_root.child("ObjectID").text().as_string();
        auto bookMarkPos = std::to_string(stoiString(req_root.child("PosSecond").text().as_string()) / divider);
        auto categoryType = req_root.child("CategoryType").text().as_string();
        auto rID = req_root.child("RID").text().as_string();

        log_info("saveSamsungBookMarkedPosition: ObjectID [{}] PosSecond [{}] CategoryType [{}] RID [{}]", objectID, bookMarkPos, categoryType, rID);

        std::map<std::string, std::string> m = {
            { "bookmarkpos", bookMarkPos },
        };
        content->updateObject(stoiString(objectID), m);
    }
    auto response = UpnpXMLBuilder::createResponse(request->getActionName(), UPNP_DESC_CDS_SERVICE_TYPE);
    request->setResponse(response);
}
