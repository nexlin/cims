#include "csc/id_token.h"

#include "mcdata/sds_codec.h"   // base64Decode
#include "util/json_lite.h"

namespace cimsue {
namespace idtoken {

using namespace jsonlite;

std::string base64UrlDecode(const std::string& s) {
    std::string b = s;
    for (auto& c : b) { if (c == '-') c = '+'; else if (c == '_') c = '/'; }
    while (b.size() % 4) b += '=';
    return mcdata::base64Decode(b);
}

static bool fail(std::string* why, const std::string& msg) {
    if (why) *why = msg;
    return false;
}

bool validate(const std::string& idToken, const std::string& issuer, const std::string& clientId,
              const std::string& nonce, int64_t nowEpoch, std::string* why) {
    size_t d1 = idToken.find('.');
    size_t d2 = d1 == std::string::npos ? d1 : idToken.find('.', d1 + 1);
    if (idToken.empty()) return fail(why, "id_token missing");
    if (d2 == std::string::npos) return fail(why, "id_token is not a JWT");
    Json j(base64UrlDecode(idToken.substr(d1 + 1, d2 - d1 - 1)));
    if (!j.root || j.root->t != JVal::Obj) return fail(why, "id_token payload is not JSON");

    // iss — 발급자와 정확히 같아야 한다(§3.1.3.7 2))
    if (Json::str(j.root, "iss") != issuer || issuer.empty()) return fail(why, "iss does not match the issuer");
    // aud — 내 client_id 가 있어야 하고, 다른 audience 는 믿지 않는다(§3.1.3.7 3))
    const JVal* aud = Json::child(j.root, "aud");
    bool mine = false, other = false;
    if (aud && aud->t == JVal::Str) mine = aud->s == clientId;
    else Json::each(aud, [&](const JVal* a) { if (a->t == JVal::Str && a->s == clientId) mine = true; else other = true; });
    if (!mine || other) return fail(why, "aud does not match the client_id");
    // exp — 지금이 그 전이어야 한다(§3.1.3.7 9))
    const JVal* exp = Json::child(j.root, "exp");
    if (!exp || exp->t != JVal::Num) return fail(why, "exp missing");
    if (nowEpoch >= (int64_t)exp->n + kClockLeewaySec) return fail(why, "id_token expired (check the device clock)");
    // nonce — 보냈으면 같은 값이 실려 있어야 한다(§3.1.3.7 11))
    if (!nonce.empty() && Json::str(j.root, "nonce") != nonce) return fail(why, "nonce does not match");
    return true;
}

}  // namespace idtoken
}  // namespace cimsue
