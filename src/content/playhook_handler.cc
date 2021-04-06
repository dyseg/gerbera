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
    if (!std::count(content->lastOpened.begin(), content->lastOpened.end(), obj)) {
        log_info("Adding '{}' to the last played list", obj->getTitle());
        content->lastOpened.push_back(obj);
        //    if(content->lastOpened.size > 5)
    } else
        log_info("'{}' is already present in the last played list", obj->getTitle());
    log_info("end");
}
