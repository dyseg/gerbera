#ifndef PLAYHOOK_HANDLER_H_INCLUDED
#define PLAYHOOK_HANDLER_H_INCLUDED

#include <memory>
class CdsObject;
class ContentManager;

class PlayHookHandler {
public:
    PlayHookHandler(std::shared_ptr<ContentManager> content, std::shared_ptr<CdsObject> obj);
    void operator()();

private:
    std::shared_ptr<ContentManager> content;
    std::shared_ptr<CdsObject> obj;
};

#endif // PLAYHOOK_HANDLER_H_INCLUDED
