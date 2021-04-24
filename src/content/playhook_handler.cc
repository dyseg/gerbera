#include "playhook_handler.h"
#include "content/content_manager.h"

PlayHookHandler::PlayHookHandler(std::shared_ptr<ContentManager> content, std::shared_ptr<CdsObject> obj)
    : content(content)
    , obj(obj)
{
}

void PlayHookHandler::operator()()
{
    log_info("start");
    content->triggerPlayHook(obj);

    auto itr = std::find(content->lastOpened.begin(), content->lastOpened.end(), obj->getParentID());

    if (itr != content->lastOpened.cend()) { //element already in vector
        log_info("Parent of '{}' is already present in the last played list", obj->getTitle());
        content->lastOpened.erase(itr);
    } else
        log_info("Adding parent of '{}' as first element to the last played list", obj->getTitle());
    content->lastOpened.insert(content->lastOpened.begin(), obj->getParentID());

    constexpr int limit = 5;
    if (content->lastOpened.size() > limit) {
        log_info("There are more than {} elements in the last played list. Removing oldest one", limit);
        content->lastOpened.pop_back();
    }

    log_info("end");
}
