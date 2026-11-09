#include "common/protocol.h"

#include <map>
#include <sstream>

namespace proto {
namespace {

std::string escape_value(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (char ch : value) {
        switch (ch) {
            case '\\':
                result += "\\\\";
                break;
            case '\n':
                result += "\\n";
                break;
            case '\r':
                result += "\\r";
                break;
            default:
                result += ch;
                break;
        }
    }
    return result;
}

std::string unescape_value(const std::string& value, std::string& error) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '\\') {
            result += value[i];
            continue;
        }
        if (i + 1 >= value.size()) {
            error = "escape sequence is not finished";
            return {};
        }
        const char next = value[++i];
        if (next == 'n') {
            result += '\n';
        } else if (next == 'r') {
            result += '\r';
        } else if (next == '\\') {
            result += '\\';
        } else {
            error = "unknown escape sequence";
            return {};
        }
    }
    return result;
}

bool parse_int(const std::string& text, int& value) {
    try {
        std::size_t used = 0;
        const int parsed = std::stoi(text, &used);
        if (used != text.size()) {
            return false;
        }
        value = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_int64(const std::string& text, std::int64_t& value) {
    try {
        std::size_t used = 0;
        const long long parsed = std::stoll(text, &used);
        if (used != text.size()) {
            return false;
        }
        value = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace

std::string encode(const Message& message) {
    std::ostringstream out;
    out << "type=" << message.type << '\n';
    out << "ver=" << message.version << '\n';
    out << "id=" << escape_value(message.sender) << '\n';
    if (!message.msgid.empty()) {
        out << "msgid=" << escape_value(message.msgid) << '\n';
    }
    if (!message.dest.empty()) {
        out << "dest=" << escape_value(message.dest) << '\n';
    }
    if (message.type == "CHAT") {
        out << "ttl=" << message.ttl << '\n';
        out << "text=" << escape_value(message.text) << '\n';
        if (message.sent_us > 0) {
            out << "sent_us=" << message.sent_us << '\n';
        }
    } else if (!message.text.empty()) {
        out << "text=" << escape_value(message.text) << '\n';
    }
    if (!message.code.empty()) {
        out << "code=" << escape_value(message.code) << '\n';
    }
    return out.str();
}

bool decode(const std::string& payload, Message& message, std::string& error) {
    if (payload.empty()) {
        error = "empty payload";
        return false;
    }
    if (payload.size() > kMaxPayload) {
        error = "payload is too large";
        return false;
    }

    std::map<std::string, std::string> fields;
    std::istringstream input(payload);
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        const std::size_t separator = line.find('=');
        if (separator == std::string::npos) {
            error = "field has no '=' separator";
            return false;
        }
        const std::string key = line.substr(0, separator);
        const std::string value = line.substr(separator + 1);
        if (key.empty()) {
            error = "empty field name";
            return false;
        }
        if (!fields.emplace(key, value).second) {
            error = "duplicate field '" + key + "'";
            return false;
        }
    }

    if (fields.count("type") == 0 || fields.count("ver") == 0 ||
        fields.count("id") == 0) {
        error = "required field is missing";
        return false;
    }

    int version = 0;
    if (!parse_int(fields["ver"], version) || version != kVersion) {
        error = "unsupported protocol version";
        return false;
    }

    Message parsed;
    parsed.type = fields["type"];
    parsed.version = version;
    parsed.sender = unescape_value(fields["id"], error);
    if (!error.empty()) {
        return false;
    }
    if (parsed.sender.empty()) {
        error = "sender id is empty";
        return false;
    }

    if (parsed.type == "HELLO") {
        message = parsed;
        return true;
    }

    if (parsed.type == "CHAT") {
        if (fields.count("msgid") == 0 || fields.count("dest") == 0 ||
            fields.count("ttl") == 0 || fields.count("text") == 0) {
            error = "CHAT field is missing";
            return false;
        }
        parsed.msgid = unescape_value(fields["msgid"], error);
        if (!error.empty()) {
            return false;
        }
        parsed.dest = unescape_value(fields["dest"], error);
        if (!error.empty()) {
            return false;
        }
        parsed.text = unescape_value(fields["text"], error);
        if (!error.empty()) {
            return false;
        }
        if (parsed.msgid.empty()) {
            error = "message id is empty";
            return false;
        }
        if (parsed.dest.empty()) {
            error = "destination is empty";
            return false;
        }
        if (!parse_int(fields["ttl"], parsed.ttl) || parsed.ttl < 0 ||
            parsed.ttl > kMaxTtl) {
            error = "invalid TTL";
            return false;
        }
        if (parsed.text.size() > static_cast<std::size_t>(kMaxTextBytes)) {
            error = "text is too long";
            return false;
        }
        if (fields.count("sent_us") != 0) {
            if (!parse_int64(fields["sent_us"], parsed.sent_us) ||
                parsed.sent_us < 0) {
                error = "invalid sent_us";
                return false;
            }
        }
        message = parsed;
        return true;
    }

    if (parsed.type == "ERROR") {
        if (fields.count("code") == 0) {
            error = "ERROR code is missing";
            return false;
        }
        parsed.code = unescape_value(fields["code"], error);
        if (!error.empty()) {
            return false;
        }
        if (fields.count("text") != 0) {
            parsed.text = unescape_value(fields["text"], error);
            if (!error.empty()) {
                return false;
            }
        }
        message = parsed;
        return true;
    }

    error = "unknown message type '" + parsed.type + "'";
    return false;
}

}  // namespace proto
