#include "session/config/convo_info_volatile.hpp"

#include <oxenc/base32z.h>
#include <oxenc/base64.h>
#include <oxenc/hex.h>
#include <oxenc/variant.h>
#include <sodium/crypto_generichash_blake2b.h>

#include <charconv>
#include <iterator>
#include <stdexcept>
#include <variant>

#include "internal.hpp"
#include "session/config/convo_info_volatile.h"
#include "session/config/error.h"
#include "session/export.h"
#include "session/types.hpp"
#include "session/util.hpp"

using namespace std::literals;
using session::ustring_view;

namespace {

void check_session_id(std::string_view session_id) {
    if (session_id.size() != 66 || !oxenc::is_hex(session_id))
        throw std::invalid_argument{
                "Invalid pubkey: expected 66 hex digits, got " + std::to_string(session_id.size()) +
                " and/or not hex"};
}

std::string session_id_to_bytes(std::string_view session_id) {
    check_session_id(session_id);
    return oxenc::from_hex(session_id);
}

void check_open_group_pubkey(std::string_view pk) {
    if (pk.size() != 64 || !oxenc::is_hex(pk))
        throw std::invalid_argument{"Invalid open group pubkey: expected 64 hex digits"};
}

std::string open_group_pubkey_to_bytes(std::string_view o_g_pk_hex) {
    check_open_group_pubkey(o_g_pk_hex);
    return oxenc::from_hex(o_g_pk_hex);
}

void make_lc(std::string& s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z')
            c += ('a' - 'A');
}

// Digs into a dict to get out an int64_t; nullopt if not there (or not int)
std::optional<int64_t> maybe_int(const session::config::dict& d, const char* key) {
    if (auto it = d.find(key); it != d.end())
        if (auto* sc = std::get_if<session::config::scalar>(&it->second))
            if (auto* i = std::get_if<int64_t>(sc))
                return *i;
    return std::nullopt;
}

session::ustring decode_pubkey(std::string_view pk) {
    session::ustring pubkey;
    pubkey.reserve(32);
    if (pk.size() == 64 && oxenc::is_hex(pk))
        oxenc::from_hex(pk.begin(), pk.end(), std::back_inserter(pubkey));
    else if ((pk.size() == 43 || (pk.size() == 44 && pk.back() == '=') && oxenc::is_base64(pk)))
        oxenc::from_base64(pk.begin(), pk.end(), std::back_inserter(pubkey));
    else if (pk.size() == 52 && oxenc::is_base32z(pk))
        oxenc::from_base32z(pk.begin(), pk.end(), std::back_inserter(pubkey));
    else
        throw std::invalid_argument{"Invalid SOGS encoded pubkey: expected hex, base32z or base64"};
    return pubkey;
}

}  // namespace

namespace session::config {

namespace convo {

    one_to_one::one_to_one(std::string&& sid) : session_id{std::move(sid)} {
        check_session_id(session_id);
    }
    one_to_one::one_to_one(std::string_view sid) : session_id{sid} { check_session_id(session_id); }
    one_to_one::one_to_one(const struct convo_info_volatile_1to1& c) :
            base{c.last_read, c.unread}, session_id{c.session_id, 66} {}

    void one_to_one::into(convo_info_volatile_1to1& c) const {
        std::memcpy(c.session_id, session_id.data(), 67);
        c.last_read = last_read;
        c.unread = unread;
    }

    open_group::open_group(
            std::string_view base_url_, std::string_view room_, ustring_view pubkey_) {
        set_server(base_url_, room_, pubkey_);
    }

    open_group::open_group(
            std::string_view base_url_, std::string_view room_, std::string_view pubkey_hex_) {
        set_server(base_url_, room_, pubkey_hex_);
    }

    open_group::open_group(const struct convo_info_volatile_open& c) : base{c.last_read, c.unread} {
        set_server(c.base_url, c.room, ustring_view{c.pubkey, 32});
    }

    void open_group::set_server(
            std::string_view new_base_url, std::string_view new_room, ustring_view new_pubkey) {
        base_url_ = canonical_url(new_base_url);
        room_ = canonical_room(new_room);
        set_pubkey(new_pubkey);
    }

    void open_group::set_server(
            std::string_view new_base_url,
            std::string_view new_room,
            std::string_view new_pubkey_encoded) {
        base_url_ = canonical_url(new_base_url);
        room_ = canonical_room(new_room);
        set_pubkey(new_pubkey_encoded);
    }

    void open_group::set_server(std::string_view full_url) {
        auto [b_url, r_token, s_pubkey] = parse_full_url(full_url);
        base_url_ = std::move(b_url);
        room_ = std::move(r_token);
        pubkey_ = std::move(s_pubkey);
    }

    void open_group::set_pubkey(ustring_view pubkey) {
        if (pubkey.size() != 32)
            throw std::invalid_argument{"Invalid pubkey: expected a 32-byte pubkey"};
        pubkey_ = pubkey;
    }
    void open_group::set_pubkey(std::string_view pubkey) { pubkey_ = decode_pubkey(pubkey); }

    std::string open_group::pubkey_hex() const {
        const auto& pk = pubkey();
        return oxenc::to_hex(pk.begin(), pk.end());
    }

    void open_group::into(convo_info_volatile_open& c) const {
        static_assert(sizeof(c.base_url) == MAX_URL + 1);
        static_assert(sizeof(c.room) == MAX_ROOM + 1);
        copy_c_str(c.base_url, base_url());
        copy_c_str(c.room, room());
        std::memcpy(c.pubkey, pubkey().data(), 32);
        c.last_read = last_read;
        c.unread = unread;
    }

    legacy_closed_group::legacy_closed_group(std::string&& cgid) : id{std::move(cgid)} {
        check_session_id(id);
    }
    legacy_closed_group::legacy_closed_group(std::string_view cgid) : id{cgid} {
        check_session_id(id);
    }
    legacy_closed_group::legacy_closed_group(const struct convo_info_volatile_legacy_closed& c) :
            base{c.last_read, c.unread}, id{c.group_id, 66} {}

    void legacy_closed_group::into(convo_info_volatile_legacy_closed& c) const {
        std::memcpy(c.group_id, id.data(), 67);
        c.last_read = last_read;
        c.unread = unread;
    }

    void base::load(const dict& info_dict) {
        last_read = maybe_int(info_dict, "r").value_or(0);
        unread = (bool)maybe_int(info_dict, "u").value_or(0);
    }

    // returns protocol, host, port.  Port can be empty; throws on unparseable values.  protocol and
    // host get normalized to lower-case.  Port will be 0 if not present in the URL, or if set to
    // the default for the protocol. The URL must not include a path (though a single optional `/`
    // after the domain is accepted and ignored).
    std::tuple<std::string, std::string, uint16_t> parse_url(std::string_view url) {
        std::tuple<std::string, std::string, uint16_t> result{};
        auto& [proto, host, port] = result;
        if (auto pos = url.find("://"); pos != std::string::npos) {
            auto proto_name = url.substr(0, pos);
            url.remove_prefix(proto_name.size() + 3);
            if (string_iequal(proto_name, "http"))
                proto = "http://";
            else if (string_iequal(proto_name, "https"))
                proto = "https://";
        }
        if (proto.empty())
            throw std::invalid_argument{"Invalid open group URL: invalid/missing protocol://"};

        bool next_allow_dot = false;
        bool has_dot = false;
        while (!url.empty()) {
            auto c = url.front();
            if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == '-') {
                host += c;
                next_allow_dot = true;
            } else if (c >= 'A' && c <= 'Z') {
                host += c + ('a' - 'A');
                next_allow_dot = true;
            } else if (next_allow_dot && c == '.') {
                host += '.';
                has_dot = true;
                next_allow_dot = false;
            } else {
                break;
            }
            url.remove_prefix(1);
        }
        if (host.size() < 4 || !has_dot || host.back() == '.')
            throw std::invalid_argument{"Invalid open group URL: invalid hostname"};

        if (!url.empty() && url.front() == ':') {
            url.remove_prefix(1);
            if (auto [p, ec] = std::from_chars(url.data(), url.data() + url.size(), port);
                ec == std::errc{})
                url.remove_prefix(p - url.data());
            else
                throw std::invalid_argument{"Invalid open group URL: invalid port"};
            if ((port == 80 && proto == "http://") || (port == 443 && proto == "https://"))
                port = 0;
        }

        if (!url.empty() && url.front() == '/')
            url.remove_prefix(1);

        // We don't (currently) allow a /path in a SOGS URL
        if (!url.empty())
            throw std::invalid_argument{"Invalid open group URL: found unexpected trailing value"};

        return result;
    }

    void open_group::canonicalize_url(std::string& url) { url = canonical_url(url); }
    void open_group::canonicalize_room(std::string& room) {
        for (auto& c : room)
            if (c >= 'A' && c <= 'Z')
                c += ('a' - 'A');
        if (room.size() > MAX_ROOM)
            throw std::invalid_argument{"Invalid open group room: room token is too long"};
        if (room.empty())
            throw std::invalid_argument{"Invalid open group room: room token cannot be empty"};
        if (room.find_first_not_of("-0123456789_abcdefghijklmnopqrstuvwxyz") !=
            std::string_view::npos)
            throw std::invalid_argument{"Invalid SOGS URL: room token contains invalid characters"};
    }
    std::string open_group::canonical_url(std::string_view url) {
        const auto& [proto, host, port] = parse_url(url);
        std::string result;
        result += proto;
        result += host;
        if (port != 0) {
            result += ':';
            result += std::to_string(port);
        }
        if (result.size() > MAX_URL)
            throw std::invalid_argument{"Invalid open group URL: base URL is too long"};
        return result;
    }
    std::string open_group::canonical_room(std::string_view room) {
        std::string r{room};
        canonicalize_room(r);
        return r;
    }

    static constexpr std::string_view qs_pubkey = "?public_key="sv;

    std::tuple<std::string, std::string, ustring> open_group::parse_full_url(
            std::string_view full_url) {
        std::tuple<std::string, std::string, ustring> result;
        auto& [base_url, room_token, pubkey] = result;

        // Consume the URL from back to front; first the public key:
        if (auto pos = full_url.rfind(qs_pubkey); pos != std::string_view::npos) {
            auto pk = full_url.substr(pos + qs_pubkey.size());
            pubkey = decode_pubkey(pk);
            full_url = full_url.substr(0, pos);
        } else {
            throw std::invalid_argument{"Invalid SOGS URL: no valid server pubkey"};
        }

        // Now look for /r/TOKEN or /TOKEN:
        if (auto pos = full_url.rfind("/r/"); pos != std::string_view::npos) {
            room_token = full_url.substr(pos + 3);
            full_url = full_url.substr(0, pos);
        } else if (pos = full_url.rfind("/"); pos != std::string_view::npos) {
            room_token = full_url.substr(pos + 1);
            full_url = full_url.substr(0, pos);
        }

        base_url = canonical_url(full_url);
        canonicalize_room(room_token);

        return result;
    }

}  // namespace convo

ConvoInfoVolatile::ConvoInfoVolatile(
        ustring_view ed25519_secretkey, std::optional<ustring_view> dumped) :
        ConfigBase{dumped} {
    load_key(ed25519_secretkey);
}

std::optional<convo::one_to_one> ConvoInfoVolatile::get_1to1(std::string_view pubkey_hex) const {
    std::string pubkey = session_id_to_bytes(pubkey_hex);

    auto* info_dict = data["1"][pubkey].dict();
    if (!info_dict)
        return std::nullopt;

    auto result = std::make_optional<convo::one_to_one>(std::string{pubkey_hex});
    result->load(*info_dict);
    return result;
}

convo::one_to_one ConvoInfoVolatile::get_or_construct_1to1(std::string_view pubkey_hex) const {
    if (auto maybe = get_1to1(pubkey_hex))
        return *std::move(maybe);

    return convo::one_to_one{std::string{pubkey_hex}};
}

std::optional<convo::open_group> ConvoInfoVolatile::get_open(
        std::string_view base_url, std::string_view room, ustring_view pubkey) const {
    auto result = std::make_optional<convo::open_group>(base_url, room, pubkey);

    if (auto* info_dict = open_field(*result).dict())
        result->load(*info_dict);
    else
        result.reset();

    return result;
}

std::optional<convo::open_group> ConvoInfoVolatile::get_open(
        std::string_view base_url, std::string_view room, std::string_view pubkey_hex) const {
    auto result = std::make_optional<convo::open_group>(base_url, room, pubkey_hex);

    if (auto* info_dict = open_field(*result).dict())
        result->load(*info_dict);
    else
        result.reset();

    return result;
}

convo::open_group ConvoInfoVolatile::get_or_construct_open(
        std::string_view base_url, std::string_view room, ustring_view pubkey) const {
    convo::open_group result{base_url, room, pubkey};

    if (auto* info_dict = open_field(result).dict())
        result.load(*info_dict);

    return result;
}

convo::open_group ConvoInfoVolatile::get_or_construct_open(
        std::string_view base_url, std::string_view room, std::string_view pubkey_hex) const {
    convo::open_group result{base_url, room, pubkey_hex};

    if (auto* info_dict = open_field(result).dict())
        result.load(*info_dict);

    return result;
}

std::optional<convo::legacy_closed_group> ConvoInfoVolatile::get_legacy_closed(
        std::string_view pubkey_hex) const {
    std::string pubkey = session_id_to_bytes(pubkey_hex);

    auto* info_dict = data["C"][pubkey].dict();
    if (!info_dict)
        return std::nullopt;

    auto result = std::make_optional<convo::legacy_closed_group>(std::string{pubkey_hex});
    result->load(*info_dict);
    return result;
}

convo::legacy_closed_group ConvoInfoVolatile::get_or_construct_legacy_closed(
        std::string_view pubkey_hex) const {
    if (auto maybe = get_legacy_closed(pubkey_hex))
        return *std::move(maybe);

    return convo::legacy_closed_group{std::string{pubkey_hex}};
}

void ConvoInfoVolatile::set(const convo::one_to_one& c) {
    auto info = data["1"][session_id_to_bytes(c.session_id)];
    set_base(c, info);
}

void ConvoInfoVolatile::set_base(const convo::base& c, DictFieldProxy& info) {
    auto r = info["r"];

    // If we're making the last_read value *older* for some reason then ignore the prune cutoff
    // (because we might be intentionally resetting the value after a deletion, for instance).
    if (auto* val = r.integer(); val && c.last_read < *val)
        r = c.last_read;
    else {
        std::chrono::system_clock::time_point last_read{std::chrono::milliseconds{c.last_read}};
        if (last_read > std::chrono::system_clock::now() - PRUNE_LOW)
            info["r"] = c.last_read;
    }

    if (c.unread)
        info["u"] = 1;
    else
        info["u"].erase();
}

std::pair<ustring, seqno_t> ConvoInfoVolatile::push() {
    // Prune off any conversations with last_read timestamps more than PRUNE_HIGH ago (unless they
    // also have a `unread` flag set, in which case we keep them indefinitely).
    const int64_t cutoff =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                    (std::chrono::system_clock::now() - PRUNE_HIGH).time_since_epoch())
                    .count();

    for (auto it = begin(); it != end();)
        std::visit(
                [&it, &cutoff, this](const auto& convo) {
                    if (!convo.unread && convo.last_read < cutoff)
                        it = erase(it);
                    else
                        ++it;
                },
                *it);

    return ConfigBase::push();
}

void ConvoInfoVolatile::set(const convo::open_group& c) {
    auto info = open_field(c);
    data["o"][c.base_url()]["#"] = c.pubkey();
    set_base(c, info);
}

void ConvoInfoVolatile::set(const convo::legacy_closed_group& c) {
    auto info = data["C"][session_id_to_bytes(c.id)];
    set_base(c, info);
}

template <typename Field>
static bool erase_impl(Field convo) {
    bool ret = convo.exists();
    convo.erase();
    return ret;
}

bool ConvoInfoVolatile::erase(const convo::one_to_one& c) {
    return erase_impl(data["1"][session_id_to_bytes(c.session_id)]);
}
bool ConvoInfoVolatile::erase(const convo::open_group& c) {
    return erase_impl(open_field(c));
}
bool ConvoInfoVolatile::erase(const convo::legacy_closed_group& c) {
    return erase_impl(data["C"][session_id_to_bytes(c.id)]);
}

bool ConvoInfoVolatile::erase(const convo::any& c) {
    return var::visit([this](const auto& c) { return erase(c); }, c);
}
bool ConvoInfoVolatile::erase_1to1(std::string_view session_id) {
    return erase(convo::one_to_one{session_id});
}
bool ConvoInfoVolatile::erase_open(
        std::string_view base_url, std::string_view room, std::string_view pubkey_hex) {
    return erase(convo::open_group{base_url, room, pubkey_hex});
}
bool ConvoInfoVolatile::erase_open(
        std::string_view base_url, std::string_view room, ustring_view pubkey) {
    return erase(convo::open_group{base_url, room, pubkey});
}
bool ConvoInfoVolatile::erase_legacy_closed(std::string_view id) {
    return erase(convo::legacy_closed_group{id});
}

ConvoInfoVolatile::iterator ConvoInfoVolatile::erase(iterator it) {
    auto remove_it = it++;
    erase(*remove_it);
    return it;
}

size_t ConvoInfoVolatile::size_1to1() const {
    if (auto* d = data["1"].dict())
        return d->size();
    return 0;
}

size_t ConvoInfoVolatile::size_open() const {
    size_t count = 0;
    auto og = data["o"];
    if (auto* servers = og.dict()) {
        for (const auto& [baseurl, info] : *servers) {
            auto server = og[baseurl];
            if (!server["#"].exists<std::string>())
                continue;
            auto rooms = server["R"];
            if (auto* rd = rooms.dict())
                count += rd->size();
        }
    }
    return count;
}

size_t ConvoInfoVolatile::size_legacy_closed() const {
    if (auto* d = data["C"].dict())
        return d->size();
    return 0;
}

size_t ConvoInfoVolatile::size() const {
    return size_1to1() + size_open() + size_legacy_closed();
}

ConvoInfoVolatile::iterator::iterator(
        const DictFieldRoot& data, bool oneto1, bool open, bool closed) {
    if (oneto1)
        if (auto* d = data["1"].dict()) {
            _it_11 = d->begin();
            _end_11 = d->end();
        }
    if (open)
        if (auto* d = data["o"].dict()) {
            _it_open_server = d->begin();
            _end_open_server = d->end();
        }
    if (closed)
        if (auto* d = data["C"].dict()) {
            _it_lclosed = d->begin();
            _end_lclosed = d->end();
        }
    _load_val();
}

/// Load _val from the current iterator position; if it is invalid, skip to the next key until we
/// find one that is valid (or hit the end).  We also span across three different iterators: first
/// we exhaust _it_11, then _it_open, then _it_lclosed.
///
/// We *always* call this after incrementing the iterator (and after iterator initialization), and
/// this is responsible for making sure that _it_11, _it_open, etc. are only set to non-nullopt if
/// the respective sub-iterator is *not* at the end (and resetting them when we hit the end).  Thus,
/// after calling this, our "end" condition will be simply that all of the three iterators are
/// nullopt.
void ConvoInfoVolatile::iterator::_load_val() {
    while (_it_11) {
        if (*_it_11 == *_end_11) {
            _it_11.reset();
            _end_11.reset();
            break;
        }

        auto& [k, v] = **_it_11;

        if (k.size() == 33 && k[0] == 0x05) {
            if (auto* info_dict = std::get_if<dict>(&v)) {
                _val = std::make_shared<convo::any>(convo::one_to_one{oxenc::to_hex(k)});
                std::get<convo::one_to_one>(*_val).load(*info_dict);
                return;
            }
        }
        ++*_it_11;
    }

    auto next_server = [this] {
        ++*_it_open_server;
        _it_open_room.reset();
        _end_open_room.reset();
    };

    while (_it_open_server) {
        if (*_it_open_server == *_end_open_server) {
            _it_open_server.reset();
            _end_open_server.reset();
            break;
        }

        auto& [base_url, server_info] = **_it_open_server;
        auto* server_info_dict = std::get_if<dict>(&server_info);
        if (!server_info_dict) {
            next_server();
            continue;
        }

        const std::string* pubkey_raw = nullptr;
        if (auto pubkey_it = server_info_dict->find("#"); pubkey_it != server_info_dict->end())
            if (auto* pk_sc = std::get_if<scalar>(&pubkey_it->second))
                pubkey_raw = std::get_if<std::string>(pk_sc);

        if (!pubkey_raw) {
            next_server();
            continue;
        }

        ustring_view pubkey{
                reinterpret_cast<const unsigned char*>(pubkey_raw->data()), pubkey_raw->size()};

        if (!_it_open_room) {
            if (auto rit = server_info_dict->find("R");
                rit != server_info_dict->end() && std::holds_alternative<dict>(rit->second)) {
                auto& rooms_dict = std::get<dict>(rit->second);
                _it_open_room = rooms_dict.begin();
                _end_open_room = rooms_dict.end();
            } else {
                next_server();
                continue;
            }
        }

        while (_it_open_room) {
            if (*_it_open_room == *_end_open_room) {
                _it_open_room.reset();
                _end_open_room.reset();
                break;
            }

            auto& [room, data] = **_it_open_room;
            auto* data_dict = std::get_if<dict>(&data);
            if (!data_dict) {
                ++*_it_open_room;
                continue;
            }

            _val = std::make_shared<convo::any>(convo::open_group{});
            auto& og = std::get<convo::open_group>(*_val);
            try {
                og.set_server(base_url, room, pubkey);
            } catch (const std::exception& e) {
                ++*_it_open_room;
                continue;
            }
            og.load(*data_dict);
            return;
        }

        ++*_it_open_server;
    }

    while (_it_lclosed) {
        if (*_it_lclosed == *_end_lclosed) {
            _it_lclosed.reset();
            _end_lclosed.reset();
            break;
        }

        auto& [k, v] = **_it_lclosed;

        if (k.size() == 33 && k[0] == 0x05) {
            if (auto* info_dict = std::get_if<dict>(&v)) {
                _val = std::make_shared<convo::any>(convo::legacy_closed_group{oxenc::to_hex(k)});
                std::get<convo::legacy_closed_group>(*_val).load(*info_dict);
                return;
            }
        }
        ++*_it_lclosed;
    }
}

bool ConvoInfoVolatile::iterator::operator==(const iterator& other) const {
    return _it_11 == other._it_11 && _it_open_server == other._it_open_server &&
           _it_open_room == other._it_open_room && _it_lclosed == other._it_lclosed;
}

bool ConvoInfoVolatile::iterator::done() const {
    return !(_it_11 || _it_open_server || _it_open_room || _it_lclosed);
}

ConvoInfoVolatile::iterator& ConvoInfoVolatile::iterator::operator++() {
    if (_it_11)
        ++*_it_11;
    else if (_it_open_room)
        ++*_it_open_room;
    else if (_it_open_server)
        ++*_it_open_server;
    else {
        assert(_it_lclosed);
        ++*_it_lclosed;
    }
    _load_val();
    return *this;
}

}  // namespace session::config

using namespace session::config;

extern "C" {
struct convo_info_volatile_iterator {
    void* _internals;
};
}

LIBSESSION_C_API
int convo_info_volatile_init(
        config_object** conf,
        const unsigned char* ed25519_secretkey_bytes,
        const unsigned char* dumpstr,
        size_t dumplen,
        char* error) {
    return c_wrapper_init<ConvoInfoVolatile>(
            conf, ed25519_secretkey_bytes, dumpstr, dumplen, error);
}

LIBSESSION_C_API bool convo_info_volatile_get_1to1(
        const config_object* conf, convo_info_volatile_1to1* convo, const char* session_id) {
    try {
        if (auto c = unbox<ConvoInfoVolatile>(conf)->get_1to1(session_id)) {
            c->into(*convo);
            return true;
        }
    } catch (...) {
    }
    return false;
}

LIBSESSION_C_API bool convo_info_volatile_get_or_construct_1to1(
        const config_object* conf, convo_info_volatile_1to1* convo, const char* session_id) {
    try {
        unbox<ConvoInfoVolatile>(conf)->get_or_construct_1to1(session_id).into(*convo);
        return true;
    } catch (...) {
        return false;
    }
}

LIBSESSION_C_API bool convo_info_volatile_get_open(
        const config_object* conf,
        convo_info_volatile_open* og,
        const char* base_url,
        const char* room,
        unsigned const char* pubkey) {
    try {
        if (auto c = unbox<ConvoInfoVolatile>(conf)->get_open(
                    base_url, room, ustring_view{pubkey, 32})) {
            c->into(*og);
            return true;
        }
    } catch (...) {
    }
    return false;
}
LIBSESSION_C_API bool convo_info_volatile_get_or_construct_open(
        const config_object* conf,
        convo_info_volatile_open* convo,
        const char* base_url,
        const char* room,
        unsigned const char* pubkey) {
    try {
        unbox<ConvoInfoVolatile>(conf)
                ->get_or_construct_open(base_url, room, ustring_view{pubkey, 32})
                .into(*convo);
        return true;
    } catch (...) {
        return false;
    }
}

LIBSESSION_C_API bool convo_info_volatile_get_legacy_closed(
        const config_object* conf, convo_info_volatile_legacy_closed* convo, const char* id) {
    try {
        if (auto c = unbox<ConvoInfoVolatile>(conf)->get_legacy_closed(id)) {
            c->into(*convo);
            return true;
        }
    } catch (...) {
    }
    return false;
}

LIBSESSION_C_API bool convo_info_volatile_get_or_construct_legacy_closed(
        const config_object* conf, convo_info_volatile_legacy_closed* convo, const char* id) {
    try {
        unbox<ConvoInfoVolatile>(conf)->get_or_construct_legacy_closed(id).into(*convo);
        return true;
    } catch (...) {
        return false;
    }
}

LIBSESSION_C_API void convo_info_volatile_set_1to1(
        config_object* conf, const convo_info_volatile_1to1* convo) {
    unbox<ConvoInfoVolatile>(conf)->set(convo::one_to_one{*convo});
}
LIBSESSION_C_API void convo_info_volatile_set_open(
        config_object* conf, const convo_info_volatile_open* convo) {
    unbox<ConvoInfoVolatile>(conf)->set(convo::open_group{*convo});
}
LIBSESSION_C_API void convo_info_volatile_set_legacy_closed(
        config_object* conf, const convo_info_volatile_legacy_closed* convo) {
    unbox<ConvoInfoVolatile>(conf)->set(convo::legacy_closed_group{*convo});
}

LIBSESSION_C_API bool convo_info_volatile_erase_1to1(config_object* conf, const char* session_id) {
    try {
        return unbox<ConvoInfoVolatile>(conf)->erase_1to1(session_id);
    } catch (...) {
        return false;
    }
}
LIBSESSION_C_API bool convo_info_volatile_erase_open(
        config_object* conf, const char* base_url, const char* room, unsigned const char* pubkey) {
    try {
        return unbox<ConvoInfoVolatile>(conf)->erase_open(base_url, room, ustring_view{pubkey, 32});
    } catch (...) {
        return false;
    }
}
LIBSESSION_C_API bool convo_info_volatile_erase_legacy_closed(
        config_object* conf, const char* group_id) {
    try {
        return unbox<ConvoInfoVolatile>(conf)->erase_legacy_closed(group_id);
    } catch (...) {
        return false;
    }
}

LIBSESSION_C_API size_t convo_info_volatile_size(const config_object* conf) {
    return unbox<ConvoInfoVolatile>(conf)->size();
}
LIBSESSION_C_API size_t convo_info_volatile_size_1to1(const config_object* conf) {
    return unbox<ConvoInfoVolatile>(conf)->size_1to1();
}
LIBSESSION_C_API size_t convo_info_volatile_size_open(const config_object* conf) {
    return unbox<ConvoInfoVolatile>(conf)->size_open();
}
LIBSESSION_C_API size_t convo_info_volatile_size_legacy_closed(const config_object* conf) {
    return unbox<ConvoInfoVolatile>(conf)->size_legacy_closed();
}

LIBSESSION_C_API convo_info_volatile_iterator* convo_info_volatile_iterator_new(
        const config_object* conf) {
    auto* it = new convo_info_volatile_iterator{};
    it->_internals = new ConvoInfoVolatile::iterator{unbox<ConvoInfoVolatile>(conf)->begin()};
    return it;
}

LIBSESSION_C_API convo_info_volatile_iterator* convo_info_volatile_iterator_new_1to1(
        const config_object* conf) {
    auto* it = new convo_info_volatile_iterator{};
    it->_internals = new ConvoInfoVolatile::iterator{unbox<ConvoInfoVolatile>(conf)->begin_1to1()};
    return it;
}
LIBSESSION_C_API convo_info_volatile_iterator* convo_info_volatile_iterator_new_open(
        const config_object* conf) {
    auto* it = new convo_info_volatile_iterator{};
    it->_internals = new ConvoInfoVolatile::iterator{unbox<ConvoInfoVolatile>(conf)->begin_open()};
    return it;
}
LIBSESSION_C_API convo_info_volatile_iterator* convo_info_volatile_iterator_new_legacy_closed(
        const config_object* conf) {
    auto* it = new convo_info_volatile_iterator{};
    it->_internals =
            new ConvoInfoVolatile::iterator{unbox<ConvoInfoVolatile>(conf)->begin_legacy_closed()};
    return it;
}

LIBSESSION_C_API void convo_info_volatile_iterator_free(convo_info_volatile_iterator* it) {
    delete static_cast<ConvoInfoVolatile::iterator*>(it->_internals);
    delete it;
}

LIBSESSION_C_API bool convo_info_volatile_iterator_done(convo_info_volatile_iterator* it) {
    auto& real = *static_cast<ConvoInfoVolatile::iterator*>(it->_internals);
    return real.done();
}

LIBSESSION_C_API void convo_info_volatile_iterator_advance(convo_info_volatile_iterator* it) {
    ++*static_cast<ConvoInfoVolatile::iterator*>(it->_internals);
}

namespace {
template <typename Cpp, typename C>
bool convo_info_volatile_it_is_impl(convo_info_volatile_iterator* it, C* c) {
    auto& convo = **static_cast<ConvoInfoVolatile::iterator*>(it->_internals);
    if (auto* d = std::get_if<Cpp>(&convo)) {
        d->into(*c);
        return true;
    }
    return false;
}
}  // namespace

LIBSESSION_C_API bool convo_info_volatile_it_is_1to1(
        convo_info_volatile_iterator* it, convo_info_volatile_1to1* c) {
    return convo_info_volatile_it_is_impl<convo::one_to_one>(it, c);
}

LIBSESSION_C_API bool convo_info_volatile_it_is_open(
        convo_info_volatile_iterator* it, convo_info_volatile_open* c) {
    return convo_info_volatile_it_is_impl<convo::open_group>(it, c);
}

LIBSESSION_C_API bool convo_info_volatile_it_is_legacy_closed(
        convo_info_volatile_iterator* it, convo_info_volatile_legacy_closed* c) {
    return convo_info_volatile_it_is_impl<convo::legacy_closed_group>(it, c);
}

LIBSESSION_C_API void convo_info_volatile_iterator_erase(
        config_object* conf, convo_info_volatile_iterator* it) {
    auto& real = *static_cast<ConvoInfoVolatile::iterator*>(it->_internals);
    real = unbox<ConvoInfoVolatile>(conf)->erase(real);
}
