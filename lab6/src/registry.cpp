#include "registry.h"

#include <filesystem>
#include <fcntl.h>
#include <sstream>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

std::vector<std::string> split_key(const std::string& key) {
    std::vector<std::string> parts;
    std::string current;
    for (char ch : key) {
        if (ch == '/') {
            if (!current.empty()) {
                parts.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) {
        parts.push_back(current);
    }
    return parts;
}

bool all_digits(const std::string& text) {
    if (text.empty()) {
        return false;
    }
    for (char ch : text) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) {
            return false;
        }
    }
    return true;
}

bool write_all(int fd, const std::string& data) {
    size_t done = 0;
    while (done < data.size()) {
        const ssize_t count =
            write(fd, data.data() + done, data.size() - done);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        done += static_cast<size_t>(count);
    }
    return true;
}

bool read_value_file(const fs::path& path, Value& value, std::string& error) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        error = system_error("open", "reading '" + path.string() + "'");
        return false;
    }

    std::string content;
    char buffer[4096];
    while (true) {
        const ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            error = system_error("read", "reading '" + path.string() + "'");
            close(fd);
            return false;
        }
        if (count == 0) {
            break;
        }
        content.append(buffer, static_cast<size_t>(count));
    }
    close(fd);

    const size_t newline = content.find('\n');
    if (newline == std::string::npos) {
        error = "value file '" + path.string() + "' has no type header";
        return false;
    }
    std::string type;
    if (!normalize_type(content.substr(0, newline), type)) {
        error = "value file '" + path.string() + "' has unknown type";
        return false;
    }
    return parse_value(type, unescape_text(content.substr(newline + 1)), value,
                       error);
}

}  // namespace

Registry::Registry(std::string root, std::string student_id)
    : root_(std::move(root)), student_id_(std::move(student_id)) {
    if (all_digits(student_id_)) {
        while (student_id_.size() < 3) {
            student_id_.insert(student_id_.begin(), '0');
        }
    }
    student_tag_ = "Student_" + student_id_;
}

bool Registry::valid_component(const std::string& text) {
    if (text.empty() || text == "." || text == "..") {
        return false;
    }
    for (char ch : text) {
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_' &&
            ch != '-' && ch != '.') {
            return false;
        }
    }
    return true;
}

bool Registry::valid_key(const std::string& key) {
    if (key.empty() || key.front() == '/' || key.back() == '/') {
        return false;
    }
    for (const std::string& part : split_key(key)) {
        if (!valid_component(part)) {
            return false;
        }
    }
    return true;
}

std::string Registry::learning_key(const std::string& run_id) const {
    std::string run = run_id.empty() ? "Run_1" : run_id;
    if (run.rfind("Run_", 0) != 0) {
        run = "Run_" + run;
    }
    return "HKEY_CURRENT_USER/Software/OSiSP/Lab06/" + student_tag_ + "/" +
           run;
}

std::string Registry::fs_path(const std::string& key) const {
    if (!valid_key(key)) {
        return {};
    }
    fs::path path = root_;
    for (const std::string& part : split_key(key)) {
        path /= part;
    }
    return path.string();
}

bool Registry::is_learning_key(const std::string& key) const {
    const std::vector<std::string> parts = split_key(key);
    return parts.size() >= 6 && parts[0] == "HKEY_CURRENT_USER" &&
           parts[1] == "Software" && parts[2] == "OSiSP" &&
           parts[3] == "Lab06" && parts[4] == student_tag_ &&
           parts[5].rfind("Run_", 0) == 0 && parts[5].size() > 4;
}

bool Registry::key_exists(const std::string& key, std::string& error) const {
    if (!valid_key(key)) {
        error = "invalid key path '" + key + "'";
        return false;
    }
    std::error_code ec;
    const fs::file_status status = fs::status(fs_path(key), ec);
    if (ec) {
        error = ec == std::errc::no_such_file_or_directory
                    ? "key '" + key + "' does not exist"
                    : "status('" + fs_path(key) + "'): " + ec.message();
        return false;
    }
    if (!fs::is_directory(status)) {
        error = "key '" + key + "' does not exist";
        return false;
    }
    return true;
}

bool Registry::init_run(const std::string& run_id, int count, unsigned seed,
                        std::string& error) {
    const std::string key = learning_key(run_id);
    if (!is_learning_key(key) || !valid_key(key)) {
        error = "run id produces an invalid learning key";
        return false;
    }

    std::error_code ec;
    fs::create_directories(fs_path(key), ec);
    if (ec) {
        error = "create_directories('" + fs_path(key) + "'): " + ec.message();
        return false;
    }

    unsigned state = seed;
    auto next = [&state]() {
        state = state * 1103515245u + 12345u;
        return (state >> 16) & 0x7fffu;
    };

    for (int i = 0; i < count; ++i) {
        std::ostringstream name;
        name << "Item_" << std::setw(3) << std::setfill('0') << i;
        std::string type;
        std::string data;
        if (i % 4 == 0) {
            type = "REG_SZ";
            data = "text-" + std::to_string(seed) + "-" + std::to_string(i);
        } else if (i % 4 == 1) {
            type = "REG_DWORD";
            data = std::to_string(next() % 100000);
        } else if (i % 4 == 2) {
            type = "REG_QWORD";
            data = std::to_string(1000000000ULL + next());
        } else {
            type = "REG_MULTI_SZ";
            data = "alpha\nbeta\nitem-" + std::to_string(i);
        }
        Value value;
        if (!parse_value(type, data, value, error) ||
            !set_value(key, name.str(), value, error)) {
            return false;
        }
    }
    return true;
}

bool Registry::set_value(const std::string& key, const std::string& name,
                         const Value& value, std::string& error) {
    if (!is_learning_key(key)) {
        error = "write denied: key is outside the educational area";
        return false;
    }
    if (!valid_key(key)) {
        error = "invalid key path '" + key + "'";
        return false;
    }
    if (!valid_component(name)) {
        error = "invalid value name '" + name + "'";
        return false;
    }
    Value checked;
    if (!parse_value(value.type, value.data, checked, error)) {
        return false;
    }

    const std::string dir = fs_path(key);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        error = "create_directories('" + dir + "'): " + ec.message();
        return false;
    }

    std::string pattern = (fs::path(dir) / ".tmpXXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    const int fd = mkstemp(buffer.data());
    if (fd < 0) {
        error = system_error("mkstemp", "creating a temporary value file");
        return false;
    }
    const std::string temp_path(buffer.data());
    if (!write_all(fd, checked.type + "\n" + escape_text(checked.data))) {
        error = system_error("write", "writing '" + temp_path + "'");
        close(fd);
        unlink(temp_path.c_str());
        return false;
    }
    if (fsync(fd) != 0) {
        error = system_error("fsync", "flushing '" + temp_path + "'");
        close(fd);
        unlink(temp_path.c_str());
        return false;
    }
    if (close(fd) != 0) {
        error = system_error("close", "closing '" + temp_path + "'");
        unlink(temp_path.c_str());
        return false;
    }

    const std::string target = (fs::path(dir) / (name + ".val")).string();
    if (rename(temp_path.c_str(), target.c_str()) != 0) {
        error = system_error("rename", "replacing '" + target + "'");
        unlink(temp_path.c_str());
        return false;
    }
    return true;
}

bool Registry::delete_value(const std::string& key, const std::string& name,
                            std::string& error) {
    if (!is_learning_key(key)) {
        error = "delete denied: key is outside the educational area";
        return false;
    }
    if (!valid_component(name)) {
        error = "invalid value name '" + name + "'";
        return false;
    }
    const std::string target =
        (fs::path(fs_path(key)) / (name + ".val")).string();
    if (unlink(target.c_str()) != 0) {
        error = system_error("unlink", "deleting '" + target + "'");
        return false;
    }
    return true;
}

bool Registry::delete_key(const std::string& key, bool tree,
                          std::string& error) {
    if (!is_learning_key(key)) {
        error = "delete denied: only the Run_* educational area may change";
        return false;
    }
    if (!valid_key(key) || !key_exists(key, error)) {
        return false;
    }

    std::error_code ec;
    if (tree) {
        fs::remove_all(fs_path(key), ec);
    } else {
        fs::remove(fs_path(key), ec);
    }
    if (ec) {
        error = std::string(tree ? "remove_all('" : "remove('") +
                fs_path(key) + "'): " + ec.message();
        return false;
    }
    return true;
}

bool Registry::read_snapshot(const std::string& key, Snapshot& out,
                             std::string& error) const {
    if (!valid_key(key)) {
        error = "invalid key path '" + key + "'";
        return false;
    }
    if (!key_exists(key, error)) {
        return false;
    }
    out = Snapshot{};
    return scan_key(key, fs_path(key), out, error);
}

bool Registry::scan_key(const std::string& key, const std::string& dir,
                        Snapshot& out, std::string& error) const {
    out.keys.insert(key);

    std::error_code ec;
    fs::directory_iterator it(dir, ec);
    if (ec) {
        error = "directory_iterator('" + dir + "'): " + ec.message();
        return false;
    }
    for (; it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) {
            error = "directory_iterator('" + dir + "'): " + ec.message();
            return false;
        }
        const fs::path path = it->path();
        const std::string name = path.filename().string();
        if (name.empty() || name.front() == '.' ||
            !ends_with(name, ".val")) {
            continue;
        }
        Value value;
        if (!read_value_file(path, value, error)) {
            return false;
        }
        const std::string value_name = name.substr(0, name.size() - 4);
        out.values[key + "/" + value_name] = Entry{key, value_name, value};
    }
    return true;
}
