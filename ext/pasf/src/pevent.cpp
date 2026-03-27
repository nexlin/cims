#include "pevent.h"
#include <map>
#include <sstream>

class PJsonContext {
public:
    std::map<std::string, std::string> params;
    mutable std::string serialized;
    mutable bool dirty;

    PJsonContext() : dirty(true) {}
};

PJsonEvent::PJsonEvent(const std::string & name) : PEvent(name) {
    _pctx = new PJsonContext();
}

PJsonEvent::~PJsonEvent() {
    delete (PJsonContext*)_pctx;
}

bool PJsonEvent::setString(const std::string & strJson) {
    // Simple mock: assume strJson is just for storage or parsing (not implemented fully)
    // For now, identifying it's a placeholder.
    return true;
}

const std::string & PJsonEvent::getString() const {
    PJsonContext* ctx = (PJsonContext*)_pctx;
    if (ctx->dirty) {
        std::stringstream ss;
        ss << "{";
        bool first = true;
        for (auto const& [key, val] : ctx->params) {
            if (!first) ss << ", ";
            ss << "\"" << key << "\": \"" << val << "\"";
            first = false;
        }
        ss << "}";
        ctx->serialized = ss.str();
        ctx->dirty = false;
    }
    return ctx->serialized;
}

bool PJsonEvent::addParam(const std::string & keys, const std::string & val) {
    PJsonContext* ctx = (PJsonContext*)_pctx;
    ctx->params[keys] = val;
    ctx->dirty = true;
    return true;
}

bool PJsonEvent::getParam(const std::string & keys, std::string & val) const {
    PJsonContext* ctx = (PJsonContext*)_pctx;
    auto it = ctx->params.find(keys);
    if (it != ctx->params.end()) {
        val = it->second;
        return true;
    }
    return false;
}
