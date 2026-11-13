#pragma once

#include <string>

#include "common.h"

class Registry {
public:
    Registry(std::string root, std::string student_id);

    std::string learning_key(const std::string& run_id) const;
    std::string fs_path(const std::string& key) const;
    bool is_learning_key(const std::string& key) const;
    bool key_exists(const std::string& key, std::string& error) const;

    bool init_run(const std::string& run_id, int count, unsigned seed,
                  std::string& error);
    bool set_value(const std::string& key, const std::string& name,
                   const Value& value, std::string& error);
    bool delete_value(const std::string& key, const std::string& name,
                      std::string& error);
    bool delete_key(const std::string& key, bool tree, std::string& error);
    bool read_snapshot(const std::string& key, Snapshot& out,
                       std::string& error) const;

    static bool valid_component(const std::string& text);
    static bool valid_key(const std::string& key);

private:
    bool scan_key(const std::string& key, const std::string& dir,
                  Snapshot& out, std::string& error) const;

    std::string root_;
    std::string student_id_;
    std::string student_tag_;
};
