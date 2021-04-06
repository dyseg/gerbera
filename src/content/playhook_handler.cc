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
    log_info("end");
}
